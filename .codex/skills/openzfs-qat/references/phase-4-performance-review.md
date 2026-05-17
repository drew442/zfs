# Phase 4 Performance Review

Last updated: 2026-05-17.

This is a human-readable review of phase 4 performance work. It summarizes what
changed and what the measured result was. It intentionally avoids implementation
detail.

## Test Context

Primary host:

```text
pve.drewnet.online
QAT hardware: dh895xcc
Kernel: 7.0.0-3-pve
QAT level: zfs_qat_cpa_dc_level=4
Large-record test setting: zfs_qat_dc_max_buf_size=1048576
```

Primary compressible source:

```text
/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif
size: 191346108 bytes
```

The Cap-96 small-record follow-up used the restored scratch source labelled
`Image.tif_` in the benchmark CSV, size `765384432` bytes.

Important comparability note:

- The earliest phase 4 CSV measured write/copy behavior only.
- The later harness also validates data with `cmp`, so it includes readback and QAT decompression.
- Compare QAT vs software within the same table/run. Do not compare absolute latency from early CSVs directly against later harness CSVs.

## Executive Summary

- QAT compression is correct and stable for the tested matrix: all file comparisons passed and `dc_fails=0` after the 4 KiB threshold fix.
- QAT is not yet consistently faster than software gzip. The best-case level 1 comparison reached parity only at single-job `128 KiB`, where QAT was `1.0%` faster.
- Best-case level 1 QAT remained `15-16%` slower than software at single-job `256 KiB` and `1 MiB`.
- Best-case level 1 QAT remained `13-21%` slower than software under four jobs.
- QAT uses much less aggregate system CPU than software gzip. In the best-case level 1 comparison, QAT system CPU was roughly `38-45%` of software in single-job runs and `26-35%` of software under four jobs.
- Larger records now actually use QAT. Before the large-record work, `256 KiB` and `1 MiB` records silently used software fallback.
- Compression ratio is close between QAT and software in the best-case level 1 comparison. Level 4 gives QAT a small ratio advantage, but it is not the performance winner.
- A later level 1-4 matrix showed no single QAT compression level wins every case. Level 4 gives the best ratio, level 1 is generally strongest under four concurrent streams, and level 3 was fastest for single-stream 128K and 1M in that run.
- The async in-flight cap improves admission behavior but creates adaptive hybrid QAT/software rows whenever cap skips are nonzero. These rows should not be described as pure-QAT performance.
- In the Cap-96 small-record follow-up, software gzip won every four-job row and three of four single-job rows. The only QAT-labelled win was single-job `32K`, and that row was already `57.3%` QAT / `42.7%` software fallback.

## Current Latency Diagnosis

Hardware acceleration is not automatically faster for this path because OpenZFS
currently calls QAT as a synchronous per-block coprocessor. Each gzip block
builds QAT buffer metadata, maps source and destination pages, allocates
compression scratch space, submits one request, waits for completion, then
unmaps and cleans up before ZFS can continue.

Likely contributors to the observed latency:

- The compression and decompression paths wait immediately after each QAT submit, so the hardware does not get much queue depth from a single ZFS worker.
- Per-block setup overhead is high relative to 128 KiB through 1 MiB records: scatter/gather construction, page mapping, zlib header/footer handling, callback completion, and cleanup all happen for every block.
- Compression still allocates and frees a destination-sized scratch buffer for every offloaded request.
- The metadata reuse pool is active, but reuse misses still occur under concurrent workloads, so some requests still fall back to per-request allocation.
- The host QAT driver configuration appears to expose limited DC parallelism to the kernel, while software gzip can spread naturally across many EPYC cores.
- Current tests use `zfs_qat_cpa_dc_level=4`; software comparisons use ZFS `gzip-1`. QAT's slightly better compression ratio suggests it may be doing more work than the software baseline.
- The latest harness includes `cmp` readback, so QAT decompression latency is included. Earlier write-only tests were also behind software at larger records, so readback is not the only cause.

This is a measurement problem before it is a tuning problem. The next code pass
adds phase timing counters so benchmarks can separate wrapper overhead from QAT
service time.

## Recommended Continuation Plan

1. Add timing kstats around QAT scratch allocation, request setup, API submit, completion wait, and cleanup.
2. Reuse compression scratch buffers, not only QAT buffer-list metadata, if timing shows scratch allocation is material.
3. Tune or expose the reuse-slot count after measuring hit and miss rates against memory cost.
4. Benchmark `zfs_qat_cpa_dc_level=1` against level 4 to determine whether the current ratio gain is costing too much latency.
5. Inspect and tune QAT driver DC instance allocation if the hardware and QAT 1.x driver configuration allow more kernel DC concurrency.
6. Measure compression and decompression separately, then consider separate offload thresholds or disable policies for reads and writes.
7. Consider a larger asynchronous ZIO integration only after the cheaper tuning work is exhausted; that is the most likely route to full hardware utilization, but it is much more invasive.

## Phase 4 Change Timeline

| Step | Basic Change | Result |
|---|---|---|
| Initial measurement | Measured existing QAT offload window. | 4 KiB QAT caused failures; 8 KiB through 128 KiB offloaded; larger records fell back to software. |
| Threshold fix | Raised QAT gzip minimum from 4 KiB to 8 KiB. | 4 KiB no longer generated QAT failures; 8 KiB still offloaded successfully. |
| Stack page arrays | Removed three temporary per-request page-pointer allocations. | 128 KiB QAT remained slower than software, but avoided those allocations. |
| Serialized workspace attempt | Tried per-instance reusable metadata under a mutex. | Regressed latency and throughput; abandoned. |
| Instance caps | Added explicit DC/CY max-instance parameters, default 48. | No performance change intended; makes the static cap visible and testable. |
| Large-record max | Added `zfs_qat_dc_max_buf_size`, opt-in up to 1 MiB. | 256 KiB and 1 MiB records now offload to QAT with zero DC failures. |
| Benchmark harness | Added repeatable CSV harness with latency summaries and QAT counters. | Current comparisons include latency, throughput, CPU, ratio, offload counters, and correctness. |
| Reuse pool | Added lock-free per-instance buffer metadata reuse with fallback allocation. | Reuse is active, but fallback allocations still happen under concurrent work. |
| Concurrent harness | Added `JOBS` support to run multiple copy/verify streams per iteration. | 4-job tests showed software gzip still faster, despite QAT using much less system CPU. |
| Timing kstats | Added per-phase QAT DC nanosecond counters to identify latency sources. | Compression wait time dominates; scratch allocation is not the primary bottleneck. |
| Level comparison | Compared QAT level 1 and level 4 with the timing counters. | Level 1 helps larger records but lowers ratio and does not resolve the latency gap. |
| DC instance split | Tested a DC-biased QAT driver split: 2 crypto / 4 compression instead of 4 crypto / 2 compression. | Mixed result; 256K improved modestly, 128K regressed, 1M was effectively flat. Host was restored to 4 crypto / 2 compression. |
| Decompression policy | Added `zfs_qat_decompress_disable` and benchmarked QAT writes with software readback. | Improved 128K/256K latency, but QAT remained slower than full software and 1M did not benefit consistently. |
| In-flight counters | Added live and peak QAT DC in-flight counters. | ZFS already drives meaningful QAT concurrency; peak compression in-flight reached 25 with one copy stream and 50 with four streams. |
| Huffman mode | Added `zfs_qat_cpa_dc_hufftype` and compared dynamic versus static Huffman with software readback. | Static lowered accumulated QAT wait time in some larger-record cases, but elapsed results were mixed and compression ratio dropped. |
| Compression bound | Used `cpaDcDeflateCompressBound()` to size scratch space and added bound/overflow counters. | Scratch allocation dropped by about 71% with zero overflows; elapsed performance was mixed. |
| Source coalescing | Added `zfs_qat_dc_coalesce_src` and buffer-list shape counters. | Source buffers dropped to 1 per request; 128K/256K improved in this matrix, while 1M regressed. |
| Destination coalescing | Added `zfs_qat_dc_coalesce_dst` and destination coalescing counters. | Destination plus scratch output list entries dropped to one QAT output buffer; results were small and mixed in single-job testing, with modest gains under four jobs. |
| Destination coalescing reuse | Reused destination coalescing buffers from the QAT DC buffer-slot pool and increased reuse slots from 4 to 32 per DC instance. | Allocation cost dropped sharply after warmup, but elapsed results were still mixed and four-job runs regressed. |
| Compression level matrix | Compared `zfs_qat_cpa_dc_level=1..4` with source and destination coalescing disabled. | Level 4 improved ratio slightly but was not the fastest. Level 1 was usually best under four jobs; level 3 was best for single-job 128K and 1M. |
| Best-case level 1 comparison | Compared level 1 QAT against software gzip in the same benchmark window with software readback. | QAT reached parity only at single-job 128K; software remained faster at larger records and under four jobs. |
| Async ZIO path | Added opt-in callback-driven QAT gzip write compression behind `zfs_qat_dc_async=1`. | Initial smoke improved one 128K row but had many submit retries/fallbacks; default remains off. |
| Async submit retry tuning | Added retry and backoff controls for async QAT submit retries. | `8` retries with `100 us` backoff was the best single-row probe, but software still won single-job rows. |
| Async in-flight cap | Added `zfs_qat_dc_async_max_inflight=96` to skip QAT when too many async requests are in flight. | Removes submit failures and can improve concurrent 128K/256K results, but rows become adaptive hybrid QAT/software when cap skips are nonzero. |
| Cap-96 small records | Benchmarked `8K`, `16K`, `32K`, and `64K` with the async cap. | Software won every four-job row and three of four single-job rows; the only win was a mixed `32K` row. |

## Current Fair Comparison

Source CSV:

```text
/root/zfs-qat-phase4-current-compare-20260513.csv
```

Test command:

```text
ITERS=3 RECORDS="128K 256K 1M" MODES="qat sw" OUT=/root/zfs-qat-phase4-current-compare-20260513.csv /root/qat-phase4-benchmark.sh
```

### Current Latency

Lower is better.

| Record | QAT Avg | Software Avg | QAT Status |
|---|---:|---:|---:|
| 128K | 820.0 ms | 778.7 ms | 5.3% slower |
| 256K | 702.7 ms | 625.2 ms | 12.4% slower |
| 1M | 614.3 ms | 546.5 ms | 12.4% slower |

```text
Latency, average ms
128K QAT  | ##################### 820.0
128K SW   | ####################  778.7
256K QAT  | ##################    702.7
256K SW   | ################      625.2
1M   QAT  | ################      614.3
1M   SW   | ##############        546.5
```

### Current Throughput

Higher is better.

| Record | QAT | Software | QAT Status |
|---|---:|---:|---:|
| 128K | 223.7 MiB/s | 235.3 MiB/s | 4.9% lower |
| 256K | 260.4 MiB/s | 293.7 MiB/s | 11.3% lower |
| 1M | 297.5 MiB/s | 334.1 MiB/s | 11.0% lower |

```text
Throughput, MiB/s
128K QAT  | #############         223.7
128K SW   | ##############        235.3
256K QAT  | ###############       260.4
256K SW   | #################     293.7
1M   QAT  | ##################    297.5
1M   SW   | ####################  334.1
```

### Current CPU

Lower is better. This is aggregate system CPU percentage during each run.

| Record | QAT System CPU | Software System CPU | Result |
|---|---:|---:|---|
| 128K | 1.17% | 4.02% | QAT much lower |
| 256K | 1.11% | 4.57% | QAT much lower |
| 1M | 1.00% | 5.05% | QAT much lower |

```text
System CPU %
128K QAT  | #####                 1.17
128K SW   | ################      4.02
256K QAT  | ####                  1.11
256K SW   | ##################    4.57
1M   QAT  | ####                  1.00
1M   SW   | ####################  5.05
```

### Current Compression Ratio

Higher is better.

| Record | QAT Ratio | Software Ratio | Result |
|---|---:|---:|---|
| 128K | 17.11x | 16.86x | QAT slightly better |
| 256K | 21.89x | 21.53x | QAT slightly better |
| 1M | 25.45x | 25.17x | QAT slightly better |

```text
Compression ratio
128K QAT  | #############         17.11x
128K SW   | #############         16.86x
256K QAT  | #################     21.89x
256K SW   | #################     21.53x
1M   QAT  | ####################  25.45x
1M   SW   | ###################   25.17x
```

### Current Offload Health

| Record | QAT Compression Requests | DC Failures | Reuse Hits | Reuse Misses |
|---|---:|---:|---:|---:|
| 128K | 4380 | 0 | 5307 | 2940 |
| 256K | 2190 | 0 | 2662 | 1476 |
| 1M | 549 | 0 | 648 | 369 |

Interpretation:

- `dc_fails=0` is the important correctness signal.
- Reuse hits prove the new reuse pool is active.
- Reuse misses are fallback allocations, not errors. They happen when all reuse slots are busy or the request cannot use a slot.

## Concurrent Comparison

Source CSV:

```text
/root/zfs-qat-phase4-concurrent-20260513.csv
```

Test command:

```text
ITERS=3 JOBS=4 RECORDS="128K 256K 1M" MODES="qat sw" OUT=/root/zfs-qat-phase4-concurrent-20260513.csv /root/qat-phase4-benchmark.sh
```

The harness wrote and verified four copies of the TIFF source in parallel for
each iteration.

### Concurrent Latency

Lower is better.

| Record | QAT Avg | Software Avg | QAT Status |
|---|---:|---:|---:|
| 128K | 1350.7 ms | 1079.1 ms | 25.2% slower |
| 256K | 1265.3 ms | 991.0 ms | 27.7% slower |
| 1M | 1227.3 ms | 935.8 ms | 31.2% slower |

```text
4-job latency, average ms
128K QAT  | ####################  1350.7
128K SW   | ################      1079.1
256K QAT  | ###################   1265.3
256K SW   | ###############       991.0
1M   QAT  | ##################    1227.3
1M   SW   | ##############        935.8
```

### Concurrent Throughput

Higher is better.

| Record | QAT | Software | QAT Status |
|---|---:|---:|---:|
| 128K | 541.8 MiB/s | 677.4 MiB/s | 20.0% lower |
| 256K | 576.9 MiB/s | 737.7 MiB/s | 21.8% lower |
| 1M | 594.9 MiB/s | 780.2 MiB/s | 23.7% lower |

```text
4-job throughput, MiB/s
128K QAT  | ##############        541.8
128K SW   | #################     677.4
256K QAT  | ###############       576.9
256K SW   | ###################   737.7
1M   QAT  | ###############       594.9
1M   SW   | ####################  780.2
```

### Concurrent CPU

Lower is better.

| Record | QAT System CPU | Software System CPU | Result |
|---|---:|---:|---|
| 128K | 2.99% | 12.50% | QAT much lower |
| 256K | 2.41% | 13.09% | QAT much lower |
| 1M | 1.84% | 13.67% | QAT much lower |

```text
4-job system CPU %
128K QAT  | ####                  2.99
128K SW   | ##################    12.50
256K QAT  | ####                  2.41
256K SW   | ###################   13.09
1M   QAT  | ###                   1.84
1M   SW   | ####################  13.67
```

### Concurrent Compression Ratio

Higher is better.

| Record | QAT Ratio | Software Ratio | Result |
|---|---:|---:|---|
| 128K | 17.16x | 16.90x | QAT slightly better |
| 256K | 21.96x | 21.60x | QAT slightly better |
| 1M | 25.56x | 25.26x | QAT slightly better |

### Concurrent Offload Health

| Record | QAT Compression Requests | DC Failures | Reuse Hits | Reuse Misses |
|---|---:|---:|---:|---:|
| 128K | 17520 | 0 | 20202 | 14376 |
| 256K | 8760 | 0 | 10057 | 7191 |
| 1M | 2196 | 0 | 2527 | 1800 |

Result: QAT remained correct under four parallel copy/verify jobs, but it did
not become faster than software gzip. The likely practical benefit is CPU
offload, not wall-clock speed, for this workload on this host.

## Timing Counter Results

Source CSVs:

```text
/root/zfs-qat-phase4-level1-timing-20260513.csv
/root/zfs-qat-phase4-level4-timing-20260513.csv
```

These runs used the same instrumented module and the same source file. The host
was returned to `zfs_qat_cpa_dc_level=4` after the level 1 test.

### Level 1 vs Level 4

| Record | Level | Avg Latency | Throughput | Ratio | Compression Wait / Request |
|---|---:|---:|---:|---:|---:|
| 128K | 1 | 859.8 ms | 212.8 MiB/s | 16.88x | 1346.1 us |
| 128K | 4 | 831.4 ms | 220.1 MiB/s | 17.11x | 1579.8 us |
| 256K | 1 | 688.9 ms | 265.4 MiB/s | 21.63x | 2679.8 us |
| 256K | 4 | 707.6 ms | 258.2 MiB/s | 21.90x | 3327.6 us |
| 1M | 1 | 586.9 ms | 311.0 MiB/s | 25.15x | 10612.6 us |
| 1M | 4 | 617.7 ms | 295.5 MiB/s | 25.45x | 13790.5 us |

Interpretation:

- Compression wait time is the dominant measured phase by a large margin.
- Scratch allocation is not currently the primary bottleneck. In the same runs it averaged roughly `2-58 us` per compression request depending on record size.
- Setup and submit overhead are visible but much smaller than completion wait. They were roughly `7-44 us` and `8-42 us` per compression request respectively.
- Level 1 reduces compression wait at `256 KiB` and `1 MiB`, but the improvement is not enough to beat software gzip and it reduces compression ratio.
- The `128 KiB` result is noisy: level 1 had lower per-request compression wait but worse wall-clock latency in this run.

### Current Direction

The timing evidence does not justify prioritizing scratch-buffer reuse as the
next change. It remains a useful cleanup, but the larger issue is that QAT
requests spend most of their cumulative time waiting for completion. The next
high-value work is to test QAT DC instance/concurrency limits and, if that is
insufficient, plan the larger asynchronous ZIO integration.

Host observation: `/etc/dh895xcc_dev0.conf` currently exposes only two
`[KERNEL_QAT]` DC instances:

```text
NumberCyInstances = 4
NumberDcInstances = 2
```

## DC Instance Split Test

Source CSVs:

```text
/root/zfs-qat-phase4-dc2-jobs4-timing-20260513.csv
/root/zfs-qat-phase4-dc4-jobs4-timing-v2-20260513.csv
```

The test changed the QAT driver `[KERNEL_QAT]` split from:

```text
NumberCyInstances = 4
NumberDcInstances = 2
```

to:

```text
NumberCyInstances = 2
NumberDcInstances = 4
```

The total kernel QAT instance count stayed at six, matching the six accelerators
reported by `adf_ctl status` for the dh895xcc device. The host was restored to
the original 4 crypto / 2 compression split after the test because the result
was mixed and not strong enough to justify reducing crypto/checksum capacity.

| Record | DC=2 Latency | DC=4 Latency | Latency Change | DC=2 Throughput | DC=4 Throughput | Throughput Change | DC=2 Comp Wait / Request | DC=4 Comp Wait / Request |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 128K | 1323.1 ms | 1354.2 ms | 2.4% slower | 551.7 MiB/s | 539.0 MiB/s | 2.3% lower | 3007.8 us | 3210.6 us |
| 256K | 1282.8 ms | 1235.7 ms | 3.7% faster | 569.1 MiB/s | 590.7 MiB/s | 3.8% higher | 6831.6 us | 6699.4 us |
| 1M | 1243.2 ms | 1239.1 ms | 0.3% faster | 587.1 MiB/s | 589.2 MiB/s | 0.4% higher | 27455.4 us | 26847.6 us |

Interpretation:

- More DC instances did not materially change the high-latency conclusion.
- The `256 KiB` case improved modestly, but the `128 KiB` regression and flat `1 MiB` result make this unsuitable as a default recommendation.
- The bottleneck still appears to be synchronous QAT completion wait, not simply too few DC instances.

## Decompression Policy Test

Source CSVs:

```text
/root/zfs-qat-phase4-qwrite-qread-v2-20260513.csv
/root/zfs-qat-phase4-qwrite-swread-v2-20260513.csv
/root/zfs-qat-phase4-sw-baseline-decompress-policy-v2-20260513.csv
/root/zfs-qat-phase4-qwrite-qread-jobs4-v2-20260513.csv
/root/zfs-qat-phase4-qwrite-swread-jobs4-v2-20260513.csv
/root/zfs-qat-phase4-sw-baseline-decompress-policy-jobs4-v2-20260513.csv
```

The test added a separate `zfs_qat_decompress_disable` policy so QAT gzip
compression can remain enabled while gzip decompression falls back to software.
The benchmark harness now accepts `VERIFY_MODE=qat|sw|same` and records
`verify_mode` plus `zfs_qat_decompress_disable`.

Single-job result:

| Record | QAT Read | Software Read | Software Baseline | SW Read vs QAT Read | QAT SW Read vs SW Baseline | QAT SW Read System CPU | SW Baseline System CPU |
|---|---:|---:|---:|---:|---:|---:|---:|
| 128K | 835.0 ms | 794.8 ms | 745.1 ms | 4.8% faster | 6.7% slower | 1.59% | 4.01% |
| 256K | 687.6 ms | 709.3 ms | 667.4 ms | 3.2% slower | 6.3% slower | 1.55% | 4.52% |
| 1M | 620.2 ms | 635.0 ms | 583.1 ms | 2.4% slower | 8.9% slower | 1.30% | 4.49% |

Four-job result:

| Record | QAT Read | Software Read | Software Baseline | SW Read vs QAT Read | QAT SW Read vs SW Baseline | QAT SW Read System CPU | SW Baseline System CPU |
|---|---:|---:|---:|---:|---:|---:|---:|
| 128K | 1335.0 ms | 1232.4 ms | 1075.3 ms | 7.7% faster | 14.6% slower | 4.44% | 12.67% |
| 256K | 1284.8 ms | 1193.9 ms | 989.7 ms | 7.1% faster | 20.6% slower | 3.57% | 13.30% |
| 1M | 1207.8 ms | 1183.2 ms | 921.5 ms | 2.0% faster | 28.4% slower | 3.31% | 13.22% |

Interpretation:

- Software readback removes QAT decompression requests as intended.
- It is useful for four-job latency across the tested record sizes, but it does not close the gap to full software gzip.
- It is not a universal default: single-job 256K and 1M latency regressed with software readback in the rerun.
- The parameter should remain a tunable policy option rather than a default change.

## In-Flight Concurrency Test

Source CSVs:

```text
/root/zfs-qat-phase4-inflight-jobs1-qread-v2-20260513.csv
/root/zfs-qat-phase4-inflight-jobs1-swread-v2-20260513.csv
/root/zfs-qat-phase4-inflight-jobs4-qread-v2-20260513.csv
/root/zfs-qat-phase4-inflight-jobs4-swread-v2-20260513.csv
```

The test added current and peak QAT DC in-flight kstats:

```text
dc_compress_inflight
dc_compress_inflight_max
dc_decompress_inflight
dc_decompress_inflight_max
```

The benchmark harness records these values so each run shows whether QAT was
fed by concurrent ZFS workers or mostly handled one request at a time.

The peak values are max counters since module load. The current counters returned
to zero after each run, which indicates the submitted QAT requests completed and
were not leaked.

| Workload | Record | Avg Latency | Throughput | Peak Compress In-Flight | Peak Decompress In-Flight |
|---|---:|---:|---:|---:|---:|
| 1 job, QAT read | 128K | 855.9 ms | 213.2 MiB/s | 25 | 7 |
| 1 job, QAT read | 256K | 737.9 ms | 247.4 MiB/s | 25 | 7 |
| 1 job, QAT read | 1M | 627.3 ms | 291.3 MiB/s | 25 | 7 |
| 1 job, software read | 128K | 794.7 ms | 230.6 MiB/s | 25 | prior 7 |
| 1 job, software read | 256K | 684.7 ms | 266.7 MiB/s | 25 | prior 7 |
| 1 job, software read | 1M | 632.4 ms | 288.5 MiB/s | 25 | prior 7 |
| 4 jobs, QAT read | 128K | 1313.6 ms | 555.7 MiB/s | 50 | 10 |
| 4 jobs, QAT read | 256K | 1303.3 ms | 560.1 MiB/s | 50 | 11 |
| 4 jobs, QAT read | 1M | 1262.4 ms | 578.7 MiB/s | 50 | 12 |
| 4 jobs, software read | 128K | 1243.5 ms | 587.0 MiB/s | 50 | prior 12 |
| 4 jobs, software read | 256K | 1215.4 ms | 600.6 MiB/s | 50 | prior 12 |
| 4 jobs, software read | 1M | 1160.6 ms | 629.2 MiB/s | 50 | prior 12 |

Interpretation:

- QAT is not limited to a single in-flight compression request in normal ZFS writeback.
- Even one copy stream generated peak compression in-flight of 25, and four copy streams generated peak compression in-flight of 50.
- A full asynchronous ZIO integration may reduce blocked worker time, but the evidence no longer supports "QAT is slow only because it is underfed" as the primary explanation.
- The next performance target should focus on QAT service-time choices such as Huffman mode, compression level/bias policy, or hardware/session options, while treating a full async rewrite as a larger architectural option rather than the immediate fix.

## Huffman Mode Test

Source CSVs:

```text
/root/zfs-qat-phase4-huff-dynamic-jobs1-swread-20260514.csv
/root/zfs-qat-phase4-huff-static-jobs1-swread-20260514.csv
/root/zfs-qat-phase4-huff-dynamic-jobs4-swread-20260514.csv
/root/zfs-qat-phase4-huff-static-jobs4-swread-20260514.csv
```

The test used QAT write-side compression with software readback to isolate
compression service time from QAT decompression latency.

| Jobs | Record | Dynamic Avg | Static Avg | Dynamic Ratio | Static Ratio | Static Result |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 128K | 788.0 ms | 801.1 ms | 17.11x | 16.03x | 1.7% slower |
| 1 | 256K | 696.3 ms | 735.8 ms | 21.89x | 19.53x | 5.7% slower |
| 1 | 1M | 609.6 ms | 617.6 ms | 25.47x | 21.86x | 1.3% slower |
| 4 | 128K | 1243.4 ms | 1303.1 ms | 17.16x | 16.07x | 4.8% slower |
| 4 | 256K | 1211.7 ms | 1196.6 ms | 21.96x | 19.59x | 1.2% faster |
| 4 | 1M | 1158.0 ms | 1162.6 ms | 25.56x | 21.93x | 0.4% slower |

Interpretation:

- Static Huffman is functional and produced zero DC failures in this matrix.
- Static reduced accumulated QAT compression wait time for 256K and 1M in the
  four-job run, but this did not translate into a broad elapsed-time win.
- Static materially reduced compression ratio on this source file.
- Keep `dynamic` as the default. Static remains useful as an explicit
  performance/compression-ratio policy input, but not as a default change from
  these results.

## Compression Bound Test

Source CSVs:

```text
/root/zfs-qat-phase4-bound-jobs1-swread-20260514.csv
/root/zfs-qat-phase4-bound-jobs4-swread-20260514.csv
/root/zfs-qat-phase4-bound-incompressible-20260514.csv
```

This pass replaced the full destination-sized compression scratch allocation
with a `cpaDcDeflateCompressBound()` calculation. The bound prevents QAT output
buffer overflow without giving QAT an entire second destination-sized buffer for
every compression request.

| Jobs | Record | Prior Avg | Bound Avg | Scratch Bytes | Scratch Saved | Overflow | Incompressible |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 128K | 788.0 ms | 775.0 ms | 47.9 MB | 119.5 MB | 0 | 0 |
| 1 | 256K | 696.3 ms | 689.6 ms | 47.9 MB | 119.6 MB | 0 | 0 |
| 1 | 1M | 609.6 ms | 622.2 ms | 48.0 MB | 119.9 MB | 0 | 0 |
| 4 | 128K | 1243.4 ms | 1268.1 ms | 191.7 MB | 478.1 MB | 0 | 0 |
| 4 | 256K | 1211.7 ms | 1205.2 ms | 191.5 MB | 478.2 MB | 0 | 0 |
| 4 | 1M | 1158.0 ms | 1156.8 ms | 191.9 MB | 479.7 MB | 0 | 0 |

Incompressible 64 MiB random-source check:

| Record | Avg Latency | Ratio | QAT Requests | Overflow | Incompressible | Read Verify |
|---|---:|---:|---:|---:|---:|---:|
| 128K | 529.3 ms | 1.00x | 512 | 0 | 512 | yes |
| 1M | 574.8 ms | 1.00x | 64 | 0 | 64 | yes |

Interpretation:

- The bound call succeeded for all tested requests and added only sub-millisecond cumulative overhead per run.
- Scratch allocation dropped from roughly a full destination-sized buffer to about 28.6% of destination bytes.
- Correctness and fallback behavior remained intact: incompressible random data was stored through the existing incompressible path with zero QAT overflows.
- Elapsed performance was mixed, so this is primarily a memory-pressure and overflow-observability improvement, not a proven latency fix.

## Source Coalescing Test

Source CSVs:

```text
/root/zfs-qat-phase4-coalesce-off-jobs1-swread-v3-20260514.csv
/root/zfs-qat-phase4-coalesce-on-jobs1-swread-v3-20260514.csv
/root/zfs-qat-phase4-coalesce-off-jobs4-swread-v3-20260514.csv
/root/zfs-qat-phase4-coalesce-on-jobs4-swread-v3-20260514.csv
```

The test added buffer-list shape counters and an experimental runtime toggle:

```text
zfs_qat_dc_coalesce_src=0
```

When enabled, QAT compression copies each source record into one contiguous QAT
input buffer before submission. The destination and scratch output lists remain
page-backed.

| Jobs | Record | Off Avg | On Avg | Off Src Buffers | On Src Buffers | Copy MB | Coalesce Cost | Result |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 128K | 824.4 ms | 749.0 ms | 32 | 1 | 191.4 | 44.3 ms | 9.1% faster |
| 1 | 256K | 703.4 ms | 673.9 ms | 64 | 1 | 191.4 | 45.1 ms | 4.2% faster |
| 1 | 1M | 616.6 ms | 630.9 ms | 256 | 1 | 191.9 | 62.3 ms | 2.3% slower |
| 4 | 128K | 1294.3 ms | 1289.3 ms | 32 | 1 | 765.5 | 173.0 ms | 0.4% faster |
| 4 | 256K | 1289.0 ms | 1203.3 ms | 64 | 1 | 765.5 | 169.8 ms | 6.6% faster |
| 4 | 1M | 1188.7 ms | 1217.5 ms | 256 | 1 | 767.6 | 232.8 ms | 2.4% slower |

Interpretation:

- The current non-coalesced path is highly fragmented: 128K uses 32 source buffers, 256K uses 64, and 1M uses 256.
- Source coalescing reliably reduced source buffers to 1 and had zero allocation failures in the tested matrix.
- The copy/allocation cost is large enough that coalescing is not a universal win.
- Keep coalescing disabled by default. It is a useful experimental knob for 128K/256K or future bias policies, but 1M should remain non-coalesced unless further work reduces copy cost.

## Destination Coalescing Test

Source CSVs:

```text
/root/zfs-qat-phase4-dst-coalesce-off-jobs1-r2-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-on-jobs1-r2-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-off-jobs4-r2-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-on-jobs4-r2-20260515.csv
```

The original `/nvme_scratch` source pool was not imported after the 2026-05-15
reboot, so this comparison used the lz4-backed copy of the same TIFF file:

```text
/test-hdd-pool/bench/cpu-lz4/realdata-test/2021-09-05/Scanned Documents/Image.tif
```

The test added an experimental runtime toggle:

```text
zfs_qat_dc_coalesce_dst=0
```

When enabled, QAT compression writes into one contiguous output buffer sized to
the normal destination plus deflate-bound scratch allowance, then copies the
successful compressed result back to the ZFS destination buffer. Source
coalescing remained disabled for this test.

| Jobs | Record | Off Avg | On Avg | Off Dst Total Buffers | On Dst Total Buffers | Alloc MB | Copy MB | Coalesce Cost | Result |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 128K | 893.1 ms | 863.8 ms | 37 | 1 | 205.4 | 6.5 | 29.6 ms | 3.3% faster |
| 1 | 256K | 755.0 ms | 764.8 ms | 73 | 1 | 205.4 | 6.6 | 29.4 ms | 1.3% slower |
| 1 | 1M | 732.6 ms | 733.5 ms | 289 | 1 | 205.9 | 6.7 | 65.0 ms | 0.1% slower |
| 4 | 128K | 1896.0 ms | 1884.1 ms | 37 | 1 | 821.6 | 26.1 | 118.6 ms | 0.6% faster |
| 4 | 256K | 1825.7 ms | 1776.8 ms | 73 | 1 | 821.4 | 26.3 | 120.6 ms | 2.7% faster |
| 4 | 1M | 1632.7 ms | 1611.2 ms | 289 | 1 | 823.5 | 26.7 | 269.4 ms | 1.3% faster |

Interpretation:

- Destination coalescing reliably reduced the QAT destination plus scratch list
  to one output buffer in the tested matrix and had zero allocation failures.
- The copied compressed output was small, about `6.5-6.7 MiB` for one job and
  `26-27 MiB` for four jobs, because the TIFF is highly compressible.
- The allocation and free cost is much larger than the copy-back cost. This
  cost moves into setup and cleanup, so QAT wait time is not materially reduced.
- Keep destination coalescing disabled by default. It is a useful experimental
  knob, especially for concurrent 256K/1M testing, but the gains are still
  small relative to run-to-run noise and allocation/free cost.

## Destination Coalescing Reuse Test

Source CSVs:

```text
/root/zfs-qat-phase4-dst-coalesce-reuse-smoke-r2-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-reuse-warmup-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-reuse-off-jobs1-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-reuse-on-jobs1-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-reuse-off-jobs4-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-reuse-on-jobs4-20260515.csv
```

The `/nvme_scratch` pool was recreated empty, so the TIFF source was restored
from the lz4-backed copy before this run:

```text
/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif
sha256 dec26817a6c7d6c6193c6db7e740e5d82bf19166a1ebab8f9af75cad80c01d6f
```

This pass changed destination coalescing to reuse one contiguous output buffer
from the existing QAT DC buffer-slot pool. The pool was increased from `4` to
`32` slots per DC instance because Phase 4 in-flight counters had already shown
up to `25` compression requests in flight with one copy stream and `50` with
four streams. The measured `on` runs were warmed with one prior 1M four-job
coalescing run so the result measured reuse rather than first allocation.

| Jobs | Record | Off Avg | On Avg | Off Dst Buffers | On Dst Buffers | Reuse Hits | Reuse Misses | Alloc MB | Result |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 128K | 836.6 ms | 797.9 ms | 37 | 1 | 1460 | 0 | 0.0 | 4.6% faster |
| 1 | 256K | 698.3 ms | 699.7 ms | 73 | 1 | 730 | 0 | 0.0 | 0.2% slower |
| 1 | 1M | 622.1 ms | 658.8 ms | 289 | 1 | 183 | 0 | 0.0 | 5.9% slower |
| 4 | 128K | 1262.8 ms | 1324.0 ms | 37 | 1 | 5646 | 194 | 27.2 | 4.8% slower |
| 4 | 256K | 1249.0 ms | 1255.2 ms | 73 | 1 | 2836 | 84 | 23.5 | 0.5% slower |
| 4 | 1M | 1160.5 ms | 1191.7 ms | 289 | 1 | 707 | 25 | 28.5 | 2.7% slower |

Interpretation:

- Reuse did what it was meant to do: warmed single-job runs had zero destination
  coalescing allocation bytes and four-job allocation dropped to about
  `23-29 MiB`, not hundreds of MiB.
- Reducing allocation did not make destination coalescing a clear performance
  win. Four-job elapsed time regressed at all tested record sizes.
- The dominant cost remains QAT completion wait/service time, not destination
  coalescing allocation.
- Keep destination coalescing disabled by default and do not prioritize further
  destination-buffer shaping unless a later async/queueing change makes it
  relevant again.

## Compression Level Matrix

Source CSVs:

```text
/root/zfs-qat-phase4-level1-jobs1-20260515.csv
/root/zfs-qat-phase4-level1-jobs4-20260515.csv
/root/zfs-qat-phase4-level2-jobs1-20260515.csv
/root/zfs-qat-phase4-level2-jobs4-20260515.csv
/root/zfs-qat-phase4-level3-jobs1-20260515.csv
/root/zfs-qat-phase4-level3-jobs4-20260515.csv
/root/zfs-qat-phase4-level4-jobs1-20260515.csv
/root/zfs-qat-phase4-level4-jobs4-20260515.csv
```

Test settings:

```text
zfs_qat_cpa_dc_hufftype=dynamic
zfs_qat_dc_max_buf_size=1048576
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
VERIFY_MODE=sw
ITERS=3
RECORDS="128K 256K 1M"
MODES="qat"
```

Lower elapsed time is better. Higher throughput and ratio are better.

| Jobs | Record | Fastest Level | Fastest Avg | Fastest Throughput | Level 4 Avg | Level 1 Ratio | Level 4 Ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 128K | 3 | 682.8 ms | 267.5 MiB/s | 729.8 ms | 16.88x | 17.11x |
| 1 | 256K | 1 | 639.1 ms | 286.2 MiB/s | 674.3 ms | 21.63x | 21.89x |
| 1 | 1M | 3 | 597.8 ms | 305.3 MiB/s | 682.4 ms | 25.15x | 25.47x |
| 4 | 128K | 1 | 1208.1 ms | 604.3 MiB/s | 1273.5 ms | 16.92x | 17.16x |
| 4 | 256K | 2 | 1139.8 ms | 640.7 MiB/s | 1210.5 ms | 21.70x | 21.96x |
| 4 | 1M | 1 | 1069.6 ms | 682.7 MiB/s | 1141.7 ms | 25.25x | 25.56x |

Accumulated QAT compression wait time also increased with higher levels in most
rows. Examples: single-job 1M averaged `1930.8 ms` at level 1, `2024.8 ms` at
level 2, `2213.9 ms` at level 3, and `2564.1 ms` at level 4; four-job 1M
averaged `15904.0 ms`, `16633.7 ms`, `17759.8 ms`, and `20180.0 ms`
respectively.

Interpretation:

- Level 4 should not be treated as the performance default merely because it
  gives the best compression ratio.
- Level 1 is the best candidate for a performance-biased policy because it won
  two of three four-job cases and had the lowest four-job QAT wait time for
  128K and 1M.
- Level 3 may be useful for single-stream latency on this host, but it regressed
  four-job 256K and 1M relative to levels 1 and 2.
- The measured ratio gain from level 1 to level 4 was small in this workload:
  about `1.4%` at 128K, `1.2%` at 256K, and `1.2%` at 1M.
- Keep the explicit `zfs_qat_cpa_dc_level` knob for now. A future
  performance/ratio bias can map to proven low-level settings, but should not
  hide this variability before more workloads are tested.

## Best-Case Level 1 Comparison

Source CSVs:

```text
/root/zfs-qat-phase4-level1-bestcase-jobs1-20260516.csv
/root/zfs-qat-phase4-level1-bestcase-jobs4-20260516.csv
```

Test settings:

```text
zfs_qat_cpa_dc_level=1
zfs_qat_cpa_dc_hufftype=dynamic
zfs_qat_dc_max_buf_size=1048576
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
VERIFY_MODE=sw
ITERS=3
RECORDS="128K 256K 1M"
MODES="qat sw"
```

Lower elapsed time is better. Negative QAT vs software means QAT was faster.

| Jobs | Record | QAT Avg | Software Avg | QAT vs SW | QAT Throughput | SW Throughput | QAT Sys CPU | SW Sys CPU | QAT Ratio | SW Ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 128K | 758.9 ms | 766.6 ms | 1.0% faster | 240.7 MiB/s | 238.2 MiB/s | 1.87% | 4.13% | 16.88x | 16.86x |
| 1 | 256K | 693.9 ms | 599.7 ms | 15.7% slower | 263.1 MiB/s | 304.8 MiB/s | 1.51% | 4.65% | 21.63x | 21.53x |
| 1 | 1M | 624.0 ms | 540.4 ms | 15.5% slower | 292.5 MiB/s | 337.8 MiB/s | 1.41% | 4.92% | 25.15x | 25.17x |
| 4 | 128K | 1208.2 ms | 1071.8 ms | 12.7% slower | 604.5 MiB/s | 682.4 MiB/s | 4.49% | 12.76% | 16.92x | 16.90x |
| 4 | 256K | 1131.2 ms | 994.3 ms | 13.8% slower | 645.4 MiB/s | 735.7 MiB/s | 3.95% | 13.37% | 21.70x | 21.60x |
| 4 | 1M | 1095.1 ms | 904.4 ms | 21.1% slower | 666.8 MiB/s | 807.1 MiB/s | 3.65% | 13.80% | 25.25x | 25.26x |

All rows passed verification and QAT reported `dc_fails=0`.

Interpretation:

- Level 1 is a better performance candidate than level 4, but it does not close
  the larger-record or concurrent throughput gap.
- The remaining gap is not primarily compression level, Huffman mode,
  destination allocation, or coalescing overhead.
- The next useful engineering target should be an asynchronous or queueing
  design spike that can keep QAT work in flight without blocking each ZFS
  worker on each individual block. The first-pass design is documented in
  `phase-4-async-queue-spike.md`.

## Async Cap-96 Hybrid Results

Source CSVs:

```text
/root/zfs-qat-phase4-async-cap96-jobs1-compare-20260516.csv
/root/zfs-qat-phase4-async-cap96-jobs4-compare-20260516.csv
/root/zfs-qat-phase4-async-cap96-small-jobs1-20260517.csv
/root/zfs-qat-phase4-async-cap96-small-jobs4-20260517.csv
```

The async in-flight cap is an admission-control policy. When the cap is reached,
the block uses software gzip instead of attempting another QAT submission. That
means cap-skipped benchmark rows are hybrid QAT/software rows.

QAT share is calculated as:

```text
dc_compress_async_completions_delta / dc_compress_async_submits_delta
```

Example: `1402` completions and `4438` cap skips means `1402 / 5840 = 24.0%`
QAT and `76.0%` software fallback.

### 128K Through 1M

| Jobs | Record | Async Cap-96 | Software | Async vs SW | QAT Share |
|---:|---:|---:|---:|---:|---:|
| 1 | 128K | 753.6 ms | 692.1 ms | 8.9% slower | 35.0% |
| 1 | 256K | 682.1 ms | 601.6 ms | 13.4% slower | 37.5% |
| 1 | 1M | 599.4 ms | 573.1 ms | 4.6% slower | 70.5% |
| 4 | 128K | 1073.7 ms | 1172.4 ms | 8.4% faster | 24.5% |
| 4 | 256K | 927.8 ms | 956.6 ms | 3.0% faster | 24.7% |
| 4 | 1M | 876.5 ms | 892.7 ms | 1.8% faster | 31.0% |

```text
QAT share, 4 jobs
128K | #####               24.5%
256K | #####               24.7%
1M   | ######              31.0%
```

Result: the four-job Cap-96 rows are faster than software in this one-iteration
matrix, but most blocks in those rows used software fallback. This is a useful
adaptive policy result, not proof that pure QAT is faster.

### 8K Through 64K

| Jobs | Record | Async Cap-96 | Software | Async vs SW | QAT Share | Ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 8K | 1668.8 ms | 1541.2 ms | 8.3% slower | 100.0% | 4.44x |
| 1 | 16K | 1299.7 ms | 1161.0 ms | 12.0% slower | 99.8% | 7.79x |
| 1 | 32K | 1105.5 ms | 1169.1 ms | 5.4% faster | 57.3% | 6.91x |
| 1 | 64K | 976.1 ms | 837.7 ms | 16.5% slower | 41.1% | 11.58x |
| 4 | 8K | 2422.5 ms | 2344.7 ms | 3.3% slower | 32.7% | 4.44x |
| 4 | 16K | 1791.1 ms | 1617.2 ms | 10.8% slower | 25.6% | 7.83x |
| 4 | 32K | 1791.0 ms | 1709.1 ms | 4.8% slower | 35.2% | 6.90x |
| 4 | 64K | 1339.3 ms | 1219.8 ms | 9.8% slower | 26.0% | 11.59x |

```text
Elapsed, 4 jobs, lower is better
8K  async | #################### 2422.5
8K  sw    | ###################  2344.7
16K async | ###############      1791.1
16K sw    | #############        1617.2
32K async | ###############      1791.0
32K sw    | ##############       1709.1
64K async | ###########          1339.3
64K sw    | ##########           1219.8
```

Result: smaller records did not fix QAT latency. The pure-QAT `8K` single-job
row was slower than software, and all four-job small-record rows were slower
than software while being mostly software fallback already.

## Earlier Phase 4 Measurements

### Initial 128 KiB Baseline

Source CSV:

```text
/root/zfs-qat-phase4-20260512-170610.csv
```

| Mode | 128K Elapsed | 128K Throughput | Ratio | DC Failures |
|---|---:|---:|---:|---:|
| QAT | 0.296 s | 616.49 MiB/s | 17.11x | 0 |
| Software | 0.244 s | 747.88 MiB/s | 16.86x | 0 |

Result: QAT was about `21%` slower by elapsed time at 128 KiB in the initial write-only measurement.

### Initial 4 KiB Problem

| Mode | Record | Elapsed | Throughput | Ratio | QAT Requests | DC Failures |
|---|---:|---:|---:|---:|---:|---:|
| QAT | 4K | 0.774 s | 235.76 MiB/s | 4.24x | 46716 | 6071 |
| Software | 4K | 0.798 s | 228.67 MiB/s | 5.57x | 0 | 0 |

Result: 4 KiB QAT was not acceptable because it produced thousands of DC failures. Raising the QAT gzip minimum to 8 KiB fixed this class of failure.

### Stack Allocation Tuning

Source CSV:

```text
/root/zfs-qat-phase4-alloc-20260513-084514.csv
```

| Mode | 128K Avg Elapsed | 128K Avg Throughput | Avg System CPU |
|---|---:|---:|---:|
| QAT | 0.392 s | 469.3 MiB/s | 1.79% |
| Software | 0.266 s | 693.1 MiB/s | 8.51% |

Result: the stack-array change reduced allocation work in the QAT path, but QAT was still slower than software in this single-file write test. CPU use was much lower with QAT.

### Abandoned Serialized Workspace

Source CSV:

```text
/root/zfs-qat-phase4-workspace-20260513-085342.csv
```

| Mode | 128K Avg Elapsed | 128K Avg Throughput | Avg System CPU |
|---|---:|---:|---:|
| QAT workspace attempt | 0.554 s | 329.6 MiB/s | 1.16% |
| Software | 0.325 s | 565.3 MiB/s | 7.15% |

Result: the serialized workspace saved allocations but hurt wall-clock performance. It was removed.

```text
128K QAT variants, elapsed seconds
Stack arrays only       | ##############        0.392
Serialized workspace    | ####################  0.554
```

### Large-Record Enablement

Before the large-record change, `256 KiB` and `1 MiB` records in QAT mode did not move QAT compression counters; they used software fallback.

After the large-record change:

| Source | Record | Elapsed | QAT Requests | DC Failures | Ratio |
|---|---:|---:|---:|---:|---:|
| TIFF | 256K | 776 ms | 730 | 0 | 21.89x |
| TIFF | 1M | 680 ms | 183 | 0 | 25.45x |
| Encrypted ZIP | 256K | 2537 ms | 1188 | 0 | 1.00x |
| Encrypted ZIP | 1M | 2734 ms | 297 | 0 | 1.00x |

Result: larger records now offload correctly. This was a capability and correctness improvement, not yet a speed win.

## Bottom Line

Current QAT state:

- Correct: yes.
- Larger records offload: yes, up to the tested 1 MiB opt-in limit.
- Faster than software gzip: no, not in the current fair single-file comparison.
- Current gap from software gzip: about `5-12%` slower by wall-clock latency and `5-11%` lower throughput.
- Faster under 4-job concurrency: no. The concurrent gap was about `25-31%` slower by wall-clock latency and `20-24%` lower throughput.
- CPU benefit: substantial. QAT uses roughly one quarter or less of the aggregate system CPU used by software gzip in the current comparison.
- Compression ratio: slightly better with QAT in the current comparison.

Next performance question:

The timing counters show QAT completion wait dominates. The next useful
benchmark should test whether the QAT 1.x driver can expose more useful DC
concurrency to the kernel. If it cannot, the realistic speed path is likely a
larger asynchronous ZIO integration rather than more small allocation cleanup.
