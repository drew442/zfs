# Phase 4 Performance Review

Last updated: 2026-05-18.

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

Benchmark interpretation note:

- Async rows with QAT cap skips are hybrid QAT/software rows, not pure-QAT
  measurements.
- Lower elapsed time with lower QAT share should be treated as a possible
  hybrid-policy win, not as evidence that the QAT engine became faster.
- Future summaries should report QAT byte share, QAT completion share, fallback
  share, CPU seconds per GiB, compression ratio, and failures alongside elapsed
  time.
- The detailed interpretation rules are in
  `benchmark-evaluation-methodology.md`.

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
- Future async-policy comparisons must separate hybrid-policy wins from QAT
  engine improvements. QAT byte share and CPU seconds per GiB are first-class
  metrics alongside latency, throughput, and compression ratio.
- In the Cap-96 small-record follow-up, software gzip won every four-job row and three of four single-job rows. The only QAT-labelled win was single-job `32K`, and that row was already `57.3%` QAT / `42.7%` software fallback.
- The host can run the ZFS QAT API service as six DC instances and zero crypto instances. In the larger-record Cap-96 repeat, this was near parity at four-job `128K`, slightly slower at `256K`, and faster at `1M`, but still mostly software fallback under the in-flight cap.
- The DC6 small-record repeat did not produce a win: four-job `8K` was effectively parity, while `16K`, `32K`, and `64K` remained slower than software.
- A DC6 cap sweep showed that higher caps are record-size sensitive rather than broadly better. Cap `192` helped repeated `256K`, cap `768` helped repeated `128K`, and uncapped mode reintroduced submit failures and was slower.
- A second DH895XCC card reduced QAT service/wait cost per QAT-completed MiB by
  roughly half and increased QAT byte share, but did not remove the small-record
  latency problem.
- A linear DC12 cap policy increased QAT byte share further, but elapsed time
  regressed in every tested row versus the conservative dual-card policy. The
  balanced policy now uses active DC instance count but caps at the measured DC6
  ceiling; higher caps should be benchmarked as profile behavior, not default
  behavior.
- A midpoint cap sweep found no useful higher-cap candidate for `128K` or
  `256K`. The `1M` repeat favored cap `192`, not the earlier cap `160`
  candidate, so a new explicit `throughput` cap policy now raises only `1M+`
  above the balanced DC6 ceiling.

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
| ZFS timing kstats | Added per-phase ZFS QAT DC nanosecond counters to identify wrapper latency sources. | Compression wait time dominates; scratch allocation is not the primary bottleneck. |
| QAT driver timing | Added `qat_api.ko` traditional DC timing counters exposed through `/proc/qat_dc_timing` and `/sys/kernel/debug/qat_api/dc_timing`, then added CSV capture to the phase-4 harness. | 128K smoke showed driver response-wait time dominates driver total time, with 1,460 submits/callbacks and zero TX retries/errors. |
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
| Six DC instances | Reconfigured `[KERNEL_QAT]` to `NumberCyInstances=0` and `NumberDcInstances=6`, with ZFS QAT crypto/checksum disabled. | Driver accepted the split. More QAT requests completed, but the Cap-96 policy still used mostly software fallback. Only the four-job `1M` repeat clearly beat software; small records did not. |
| DC6 cap sweep | Swept `zfs_qat_dc_async_max_inflight` over `96`, `192`, `384`, `768`, and uncapped. | Higher caps are not generally better. Cap choice is record-size dependent; uncapped mode is slower and causes submit failures. |
| Dual-card scale | Tested two DH895XCC cards with 12 configured DC instances. | QAT byte share increased and service/wait cost per QAT-completed MiB fell by roughly half, but `128K` remained slower than software. |
| Linear DC12 caps | Tested caps scaled directly from DC6 to DC12. | QAT byte share increased, but elapsed time regressed versus the conservative dual-card policy in every row. Keep DC6 ceilings for the balanced profile. |
| Midpoint cap profile sweep | Tested caps between balanced DC6 ceilings and linear DC12 endpoints at `JOBS=8`. | `128K` and `256K` do not justify higher caps. `1M` needed a repeat before policy. |
| 1M cap repeat | Repeated `1M` caps `96`, `160`, and `192` with six iterations at `JOBS=8`. | Cap `192` was fastest and lowest system CPU in this window. Added explicit `throughput` cap policy for `1M+` only. |

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

### Six DC Instances

Source CSVs:

```text
/root/zfs-qat-phase4-async-cap96-dc6-jobs1-20260517.csv
/root/zfs-qat-phase4-async-cap96-dc6-jobs4-20260517.csv
/root/zfs-qat-phase4-async-cap96-dc6-jobs4-repeat-20260517.csv
```

The host was reconfigured to use the ZFS QAT API kernel service as six data
compression instances and zero crypto instances:

```text
NumberCyInstances = 0
NumberDcInstances = 6
zfs_qat_checksum_disable=1
zfs_qat_encrypt_disable=1
```

The driver accepted the split after reboot, and the benchmark CSVs report
`qat_kernel_cy_instances=0` and `qat_kernel_dc_instances=6`.

| Jobs | Record | Async Cap-96 | Software | Async vs SW | QAT Share |
|---:|---:|---:|---:|---:|---:|
| 1 | 128K | 837.4 ms | 725.0 ms | 15.5% slower | 44.9% |
| 1 | 256K | 641.0 ms | 641.0 ms | 0.0% slower | 50.8% |
| 1 | 1M | 587.2 ms | 566.9 ms | 3.6% slower | 76.5% |
| 4 | 128K | 1079.3 ms | 1089.1 ms | 0.9% faster | 30.4% |
| 4 | 256K | 1037.0 ms | 966.3 ms | 7.3% slower | 30.0% |
| 4 | 1M | 897.3 ms | 958.8 ms | 6.4% faster | 34.2% |

Three-iteration four-job repeat:

| Record | Async Avg | Software Avg | Async vs SW | QAT Share |
|---:|---:|---:|---:|---:|
| 128K | 1150.1 ms | 1152.6 ms | 0.2% faster | 29.9% |
| 256K | 981.9 ms | 974.8 ms | 0.7% slower | 28.7% |
| 1M | 882.4 ms | 953.7 ms | 7.5% faster | 35.5% |

```text
Three-iteration jobs=4, elapsed, lower is better
128K dc6 | ############        1150.1
128K sw  | ############        1152.6
256K dc6 | ##########          981.9
256K sw  | ##########          974.8
1M   dc6 | #########           882.4
1M   sw  | ##########          953.7
```

Result: six DC instances increase the share of requests that complete through
QAT, but they do not remove the hybrid-policy problem. With Cap-96, most
four-job blocks still use software fallback, and the only clear repeated win is
the `1M` row.

### Six DC Instances, 8K Through 64K

Source CSVs:

```text
/root/zfs-qat-phase4-async-cap96-dc6-small-jobs1-20260517.csv
/root/zfs-qat-phase4-async-cap96-dc6-small-jobs4-20260517.csv
/root/zfs-qat-phase4-async-cap96-dc6-small-jobs4-repeat-20260517.csv
```

| Jobs | Record | Async Cap-96 | Software | Async vs SW | QAT Share |
|---:|---:|---:|---:|---:|---:|
| 1 | 8K | 1526.7 ms | 1522.0 ms | 0.3% slower | 99.7% |
| 1 | 16K | 1184.1 ms | 1174.1 ms | 0.9% slower | 99.9% |
| 1 | 32K | 1055.3 ms | 1076.1 ms | 1.9% faster | 67.7% |
| 1 | 64K | 954.5 ms | 892.4 ms | 7.0% slower | 40.1% |
| 4 | 8K | 2365.4 ms | 2392.1 ms | 1.1% faster | 41.5% |
| 4 | 16K | 1723.0 ms | 1603.4 ms | 7.5% slower | 28.6% |
| 4 | 32K | 1823.7 ms | 1631.3 ms | 11.8% slower | 37.5% |
| 4 | 64K | 1391.7 ms | 1211.1 ms | 14.9% slower | 34.4% |

Three-iteration four-job repeat:

| Record | Async Avg | Software Avg | Async vs SW | QAT Share |
|---:|---:|---:|---:|---:|
| 8K | 2346.5 ms | 2339.1 ms | 0.3% slower | 58.8% |
| 16K | 1713.8 ms | 1601.8 ms | 7.0% slower | 31.4% |
| 32K | 1769.3 ms | 1718.0 ms | 3.0% slower | 36.3% |
| 64K | 1358.4 ms | 1228.2 ms | 10.6% slower | 30.5% |

```text
Three-iteration jobs=4, elapsed, lower is better
8K  dc6 | #################### 2346.5
8K  sw  | #################### 2339.1
16K dc6 | ###############      1713.8
16K sw  | ##############       1601.8
32K dc6 | ###############      1769.3
32K sw  | ###############      1718.0
64K dc6 | ###########          1358.4
64K sw  | ##########           1228.2
```

Result: DC6 improves QAT participation at small records, especially single-job
`8K` and `16K`, but it does not make small records faster than software gzip.
The only repeated jobs=4 near-parity row is `8K`; the rest remain slower.

### DC6 Cap Sweep

Source CSVs:

```text
/root/zfs-qat-phase4-async-dc6-cap96-sweep-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap192-sweep-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap384-sweep-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap768-sweep-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap0-sweep-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap192-target-repeat-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap384-target-repeat-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap768-target-repeat-jobs4-20260517.csv
```

Targeted three-iteration repeats:

| Cap | Record | Async Avg | Software Avg | Async vs SW | QAT Share | Submit Fails |
|---:|---:|---:|---:|---:|---:|---:|
| 192 | 32K | 1906.1 ms | 1686.5 ms | 13.0% slower | 41.2% | 0 |
| 192 | 128K | 1072.5 ms | 1067.2 ms | 0.5% slower | 26.8% | 0 |
| 192 | 256K | 955.6 ms | 986.0 ms | 3.1% faster | 27.1% | 0 |
| 192 | 1M | 977.4 ms | 944.3 ms | 3.5% slower | 45.7% | 0 |
| 384 | 32K | 1900.4 ms | 1646.2 ms | 15.4% slower | 40.4% | 0 |
| 384 | 128K | 1084.4 ms | 1059.1 ms | 2.4% slower | 28.7% | 0 |
| 384 | 256K | 965.7 ms | 948.9 ms | 1.8% slower | 33.4% | 0 |
| 384 | 1M | 1092.0 ms | 954.1 ms | 14.4% slower | 73.4% | 0 |
| 768 | 32K | 1893.1 ms | 1669.0 ms | 13.4% slower | 37.2% | 0 |
| 768 | 128K | 1060.9 ms | 1096.3 ms | 3.2% faster | 37.5% | 0 |
| 768 | 256K | 988.4 ms | 982.3 ms | 0.6% slower | 47.6% | 0 |
| 768 | 1M | 1178.6 ms | 933.7 ms | 26.2% slower | 100.0% | 0 |

```text
Best repeated result by record, lower is better
128K cap768 | ###########         1060.9
128K sw     | ###########         1096.3
256K cap192 | ##########          955.6
256K sw     | ##########          986.0
1M   cap96  | #########           882.4
1M   sw     | ##########          953.7
```

Uncapped mode (`zfs_qat_dc_async_max_inflight=0`) was tested as a boundary case.
It removed cap skips, but it caused final submit failures on small and mid-size
records. In the one-iteration sweep, uncapped `8K` had `4872` final submit
failures and `145431` submit retries, and uncapped `1M` was still `27.7%`
slower than software despite completing all QAT submissions.

Result: higher caps increase QAT participation, but more QAT work does not
translate directly into lower latency. The best cap appears record-size
dependent, and uncapped mode should not be used for this workload.

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

### DC6 Recordsize Cap Policy

Source CSVs:

```text
/root/zfs-qat-phase4-async-dc6-policy-fixed-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-policy-recordsize-jobs4-20260517.csv
```

Policy:

```text
fixed      = use zfs_qat_dc_async_max_inflight for every eligible record
recordsize = include active DC count and measured record-size caps

DC6 recordsize caps:
<128K = software fallback
128K  = 768
256K  = 192
1M+   = 96
```

Three-iteration jobs=4 comparison:

| Policy | Record | QAT Avg | Software Avg | QAT vs Software | QAT Share |
|---|---:|---:|---:|---:|---:|
| fixed | 8K | 2432.329 ms | 2307.079 ms | 5.4% slower | 49.7% |
| fixed | 16K | 1685.861 ms | 1609.556 ms | 4.7% slower | 27.9% |
| fixed | 32K | 1857.981 ms | 1699.187 ms | 9.3% slower | 36.7% |
| fixed | 64K | 1377.161 ms | 1268.829 ms | 8.5% slower | 28.9% |
| fixed | 128K | 1090.465 ms | 1087.621 ms | 0.3% slower | 24.7% |
| fixed | 256K | 967.729 ms | 982.396 ms | 1.5% faster | 24.9% |
| fixed | 1M | 858.618 ms | 940.062 ms | 8.7% faster | 33.5% |
| recordsize | 8K | 2329.554 ms | 2297.649 ms | 1.4% slower | 0.0% |
| recordsize | 16K | 1635.570 ms | 1638.606 ms | 0.2% faster | 0.0% |
| recordsize | 32K | 1633.441 ms | 1693.153 ms | 3.5% faster | 0.0% |
| recordsize | 64K | 1243.085 ms | 1306.951 ms | 4.9% faster | 0.0% |
| recordsize | 128K | 1068.960 ms | 1084.435 ms | 1.4% faster | 35.9% |
| recordsize | 256K | 950.537 ms | 973.445 ms | 2.4% faster | 27.3% |
| recordsize | 1M | 901.087 ms | 977.362 ms | 7.8% faster | 32.2% |

Result: this is the best current default candidate for QAT 1.x async gzip
policy. Below `128K`, forcing software avoids the known QAT small-record
penalty. At `128K` and `256K`, the policy applies higher measured caps for the
six-DC host and beats software in this run. At `1M`, the policy preserves the
cap-96 behavior and remains faster than software, although this run was slower
than the same-window fixed row.

## Bottom Line

Current QAT state:

- Correct: yes.
- Larger records offload: yes, up to the tested 1 MiB opt-in limit.
- Faster than software gzip: yes for selected concurrent jobs=4 rows with the
  current async hybrid policy, not yet as a universal QAT-only path.
- Current recordsize-policy result: `16K` through `1M` beat software by
  `0.2-7.8%` in the latest DC6 jobs=4 run; `8K` remained `1.4%` slower.
- Fixed cap-96 result: still best for the latest same-window `1M` row, `8.7%`
  faster than software, but it remains slower than software below `128K`.
- CPU benefit: substantial. QAT uses roughly one quarter or less of the aggregate system CPU used by software gzip in the current comparison.
- Compression ratio: slightly better with QAT in the current comparison.

Next performance question:

The next useful benchmark is a focused validation of the `recordsize` policy,
especially the `1M` row where same-window fixed cap-96 was faster. If that row
holds, keep the `1M+` cap at `96` and treat any remaining difference as
run-to-run noise unless repeated evidence says otherwise.

Policy-matrix follow-up:

- Detailed policy matrix:
  `.codex/skills/openzfs-qat/references/phase-4-policy-matrix.md`.
- The focused six-iteration `1M` repeat showed fixed and recordsize are
  effectively the same policy at `1M`, both using cap `96`.
- Source and destination coalescing are now technically compatible with the
  async path. The first jobs=4 matrix does not support global enablement:
  `128K` regressed, `256K` favored coalescing, and `1M` favored source-only
  among QAT rows.
- A focused six-iteration repeat for `256K` and `1M` weakened the coalescing
  case further. The best `256K` row was coalescing off, and the best `1M`
  coalesced row was effectively tied with software. Keep coalescing manual for
  now.
- Compression level and Huffman type are session-global today. They are valid
  bias-profile candidates, but they cannot be selected per record without a
  multi-session QAT DC implementation.

## Methodology Scorecard Run

Run date: 2026-05-18.

This run used the updated benchmark harness with derived QAT byte-share,
fallback-share, and CPU-seconds-per-GiB columns.

Source CSV:

```text
/root/zfs-qat-phase4-methodology-current-policy-20260518.csv

Repo copy:
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-methodology-current-policy-20260518.csv
```

Settings:

```text
NumberCyInstances = 0
NumberDcInstances = 6
zfs_qat_dc_async=1
zfs_qat_dc_async_cap_policy=recordsize
zfs_qat_decompress_disable=1
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
VERIFY_MODE=sw
JOBS=4
ITERS=3
RECORDS="128K 256K 1M"
```

| Record | QAT Avg | Software Avg | QAT vs Software | QAT CPU s/GiB | SW CPU s/GiB | QAT Byte Share | Fallback Share | Outcome |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| 128K | 1065.855 ms | 1050.781 ms | 1.4% slower | 10.001 | 12.192 | 37.1% | 62.9% | CPU-offload win |
| 256K | 953.253 ms | 987.853 ms | 3.5% faster | 9.585 | 11.772 | 27.0% | 73.0% | Hybrid-policy win |
| 1M | 846.853 ms | 916.498 ms | 7.6% faster | 8.613 | 11.221 | 32.0% | 68.1% | Hybrid-policy win |

Interpretation:

- The `256K` and `1M` rows are useful policy wins, but they are not QAT engine
  wins because most bytes fell back to software.
- The `128K` row is a CPU-offload win, not a latency win.
- This confirms the need to keep elapsed-time policy evaluation separate from
  QAT engine evaluation.

## Single-Card Scale Baseline

Run date: 2026-05-18.

This is the pre-install baseline for testing whether a second DH895XCC card
raises useful QAT byte share or only exposes the same per-request bottleneck.

Source CSVs:

```text
/root/zfs-qat-scale-single-card-jobs4-20260518.csv
/root/zfs-qat-scale-single-card-jobs8-20260518.csv

Repo copies:
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-scale-single-card-jobs4-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-scale-single-card-jobs8-20260518.csv
```

Recorded scale state:

```text
qat_pci_dh895xcc_count=1
qat_conf_file_count=1
qat_kernel_cy_instances_total=0
qat_kernel_dc_instances_total=6
```

| Jobs | Record | QAT Avg | Software Avg | QAT vs Software | QAT CPU s/GiB | SW CPU s/GiB | QAT Byte Share | Fallback Share | Outcome |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---|
| 4 | 128K | 1093.410 ms | 1061.340 ms | 3.0% slower | 10.104 | 12.319 | 36.7% | 63.3% | CPU-offload win |
| 4 | 256K | 985.632 ms | 969.663 ms | 1.6% slower | 9.558 | 11.568 | 26.7% | 73.3% | CPU-offload win |
| 4 | 1M | 868.412 ms | 927.999 ms | 6.4% faster | 8.535 | 11.250 | 32.4% | 67.7% | Hybrid-policy win |
| 8 | 128K | 1561.836 ms | 1572.868 ms | 0.7% faster | 10.098 | 14.492 | 38.3% | 61.7% | CPU-offload win |
| 8 | 256K | 1459.295 ms | 1365.003 ms | 6.9% slower | 10.069 | 13.070 | 29.6% | 70.5% | CPU-offload win |
| 8 | 1M | 1631.944 ms | 1710.485 ms | 4.6% faster | 11.629 | 18.970 | 36.3% | 64.0% | Hybrid-policy win |

Dual-card success criteria:

- QAT byte share should rise materially from the single-card `26.7-38.3%`
  range.
- CPU seconds per GiB should stay lower than software and not regress versus
  the single-card QAT rows.
- QAT service and wait nanoseconds per MiB should not increase materially.
- If elapsed time improves only because software fallback changes, classify the
  result as a hybrid-policy win, not a QAT engine improvement.

## Dual-Card Scale Test

Run date: 2026-05-18.

The second DH895XCC card was installed and configured as another DC-only QAT
device. The host was rebooted before benchmarking so ZFS QAT DC initialization
could see both cards.

Recorded scale state:

```text
qat_pci_dh895xcc_count=2
qat_conf_file_count=2
qat_kernel_cy_instances_total=0
qat_kernel_dc_instances_total=12
```

Source CSVs:

```text
/root/zfs-qat-scale-dual-card-jobs4-20260518.csv
/root/zfs-qat-scale-dual-card-jobs8-20260518.csv

Repo copies:
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-scale-dual-card-jobs4-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-scale-dual-card-jobs8-20260518.csv
```

| Jobs | Record | QAT Avg | Software Avg | QAT vs Software | QAT CPU s/GiB | SW CPU s/GiB | QAT Byte Share | Fallback Share | Outcome |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---|
| 4 | 128K | 1112.824 ms | 1052.599 ms | 5.7% slower | 9.416 | 12.218 | 56.7% | 43.3% | CPU-offload win |
| 4 | 256K | 957.795 ms | 1018.140 ms | 5.9% faster | 8.400 | 11.936 | 44.7% | 55.3% | Hybrid-policy win |
| 4 | 1M | 887.080 ms | 890.151 ms | 0.3% faster | 7.484 | 11.467 | 47.1% | 53.1% | CPU-offload win |
| 8 | 128K | 1571.288 ms | 1483.652 ms | 5.9% slower | 8.691 | 13.248 | 67.2% | 32.8% | CPU-offload win |
| 8 | 256K | 1383.344 ms | 1444.475 ms | 4.2% faster | 8.274 | 13.210 | 49.0% | 51.0% | Hybrid-policy win |
| 8 | 1M | 1346.845 ms | 1455.752 ms | 7.5% faster | 8.399 | 12.132 | 48.2% | 52.1% | Hybrid-policy win |

Scale comparison versus single-card QAT:

| Jobs | Record | Elapsed Change | QAT Byte Share Change | CPU s/GiB Change | Service ns/MiB Change | Wait ns/MiB Change |
|---:|---|---:|---:|---:|---:|---:|
| 4 | 128K | 1.8% slower | +20.0 pp | 6.8% lower | 48.1% lower | 48.1% lower |
| 4 | 256K | 2.8% faster | +17.9 pp | 12.1% lower | 49.8% lower | 49.9% lower |
| 4 | 1M | 2.1% slower | +14.7 pp | 12.3% lower | 47.7% lower | 48.2% lower |
| 8 | 128K | 0.6% slower | +28.9 pp | 13.9% lower | 48.3% lower | 48.3% lower |
| 8 | 256K | 5.2% faster | +19.4 pp | 17.8% lower | 48.7% lower | 48.8% lower |
| 8 | 1M | 17.5% faster | +12.0 pp | 27.8% lower | 56.5% lower | 56.6% lower |

Interpretation:

- The second card is being used: QAT byte share increased materially and QAT
  service/wait cost per completed MiB fell by about half.
- Scale helps most at `256K` and `1M`, especially `JOBS=8`.
- Scale does not solve the `128K` latency problem, so that path is still
  dominated by per-request overhead and should remain a CPU-offload case unless
  later policy changes prove otherwise.
- Even with two cards, fallback remains high enough that these are hybrid
  policy results rather than pure-QAT throughput results.
