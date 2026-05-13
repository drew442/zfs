# Phase 4 Performance Review

Last updated: 2026-05-13.

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

Important comparability note:

- The earliest phase 4 CSV measured write/copy behavior only.
- The later harness also validates data with `cmp`, so it includes readback and QAT decompression.
- Compare QAT vs software within the same table/run. Do not compare absolute latency from early CSVs directly against later harness CSVs.

## Executive Summary

- QAT compression is correct and stable for the tested matrix: all file comparisons passed and `dc_fails=0` after the 4 KiB threshold fix.
- QAT is not yet faster than software gzip on the latest fair comparison.
- Current QAT wall-clock latency is about `5%` slower than software at `128 KiB`, and about `12%` slower at `256 KiB` and `1 MiB`.
- QAT uses much less aggregate system CPU than software gzip in the latest comparison.
- Larger records now actually use QAT. Before the large-record work, `256 KiB` and `1 MiB` records silently used software fallback.
- Compression ratio is slightly better with QAT in the latest run, likely because QAT is using level 4.

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
- CPU benefit: substantial. QAT uses roughly one quarter or less of the aggregate system CPU used by software gzip in the current comparison.
- Compression ratio: slightly better with QAT in the current comparison.

Next performance question:

The current single-file test does not show a speed win, but QAT leaves much more CPU available. The next useful benchmark should test whether that lower CPU cost turns into higher total throughput under higher concurrency or heavier mixed workloads.
