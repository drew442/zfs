# QAT Current Dual-Media Baseline - 2026-05-27

## Purpose

Create a fresh current-code baseline for request-overhead work across both NVMe and HDD media. This run is a baseline and analysis pass only; no source behavior changed.

This baseline includes:

- `nvme_scratch/bench`
- `test-hdd-pool/bench`
- Default `balanced` profile at `128K`
- `throughput` profile at `512K`
- `throughput` profile at `1M`
- Jobs `1`, `4`, and `8`
- Software verification for every row

Artifacts are in `artifacts/current-dual-media-baseline-20260527/`.

Summary files:

- `summary-raw-means.csv`: per raw-row metric means.
- `comparison-qat-vs-sw.csv`: QAT-mode versus software-mode comparisons.

## Host State

- Host: `pve.drewnet.online`
- Kernel: `7.0.0-3-pve`
- ZFS module `srcversion`: `4F680B7A990EAE933F81B16`
- QAT devices: 2x DH895XCC
- Active ZFS QAT DC instances: 12
- QAT service: active
- Pools: healthy
- All raw CSV files had consistent 254-column rows.
- All raw rows reported `sha_ok=yes`.
- Post-run host state was restored to default balanced/profile boot settings:
  - `zfs_qat_dc_profile=balanced`
  - `zfs_qat_dc_profile_recordsize=131072`
  - `zfs_qat_dc_async=profile`
  - `zfs_qat_dc_async_max_inflight=profile`
  - `zfs_qat_dc_async_cap_policy=profile`
  - `zfs_qat_dc_min_buf_size=profile`
  - `zfs_qat_dc_max_buf_size=profile`
  - `zfs_qat_decompress_disable=profile`

## Interpretation Notes

`Speedup` is `software elapsed / QAT-mode elapsed`; values above `1.000` mean QAT mode finished faster.

`CPU delta s/GiB` is `QAT-mode CPU active s/GiB - software CPU active s/GiB`; negative values mean QAT mode used less CPU.

Rows with `QAT share=0.0%` are software fallback in QAT mode, not QAT engine results. These rows are still useful because fallback behavior is part of the product requirement, but they must not be used as evidence that QAT engine performance improved.

## General Baseline Summary

| Profile | Media | Jobs | Record | QAT ms | SW ms | Speedup | CPU delta s/GiB | QAT share | Fallback |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| default | nvme | 1 | 128K | 563.1 | 562.1 | 0.998 | -5.72 | 100.0% | 0.0% |
| default | nvme | 4 | 128K | 828.1 | 765.8 | 0.925 | -7.20 | 100.0% | 0.0% |
| default | nvme | 8 | 128K | 1166.3 | 1130.0 | 0.969 | -10.20 | 100.0% | 0.0% |
| default | hdd | 1 | 128K | 813.1 | 795.1 | 0.978 | -5.00 | 100.0% | 0.0% |
| default | hdd | 4 | 128K | 1068.9 | 1066.4 | 0.998 | -7.44 | 100.0% | 0.0% |
| default | hdd | 8 | 128K | 1642.2 | 1559.5 | 0.950 | -7.08 | 100.0% | 0.0% |

Default `balanced` profile at `128K` only accelerates `128K` rows. Larger-record default rows fall back to software because effective max buffer size is `128K`; the full fallback rows are in `comparison-qat-vs-sw.csv`.

The default profile is a CPU-saving profile at `128K`, not a throughput win in this run. It saved about `5.00` to `10.20` CPU active seconds per GiB versus software, but elapsed time was parity to slower than software.

## Throughput Profile Summary

| Profile | Media | Jobs | Record | QAT ms | SW ms | Speedup | CPU delta s/GiB | QAT share | Fallback |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| throughput512 | nvme | 1 | 512K | 524.7 | 527.3 | 1.005 | -4.68 | 73.8% | 26.2% |
| throughput512 | nvme | 4 | 512K | 708.4 | 761.2 | 1.075 | -3.49 | 45.6% | 54.4% |
| throughput512 | nvme | 8 | 512K | 842.7 | 827.4 | 0.982 | -4.26 | 56.2% | 43.9% |
| throughput512 | hdd | 1 | 512K | 600.8 | 599.0 | 0.997 | -4.87 | 64.8% | 35.2% |
| throughput512 | hdd | 4 | 512K | 869.6 | 943.1 | 1.084 | -3.58 | 47.4% | 52.6% |
| throughput512 | hdd | 8 | 512K | 1228.9 | 1465.2 | 1.192 | -4.20 | 52.4% | 47.7% |
| throughput1m | nvme | 1 | 512K | 515.6 | 522.0 | 1.012 | -4.22 | 75.1% | 24.9% |
| throughput1m | nvme | 4 | 512K | 731.7 | 772.4 | 1.056 | -3.12 | 45.8% | 54.2% |
| throughput1m | nvme | 8 | 512K | 781.9 | 830.6 | 1.062 | -4.40 | 52.0% | 48.1% |
| throughput1m | hdd | 1 | 512K | 568.2 | 564.9 | 0.994 | -4.53 | 69.5% | 30.5% |
| throughput1m | hdd | 4 | 512K | 916.8 | 996.7 | 1.087 | -4.38 | 48.7% | 51.3% |
| throughput1m | hdd | 8 | 512K | 1225.2 | 1468.5 | 1.199 | -5.20 | 53.3% | 46.8% |
| throughput1m | nvme | 1 | 1M | 535.4 | 525.2 | 0.981 | -6.26 | 100.0% | 0.0% |
| throughput1m | nvme | 4 | 1M | 778.3 | 710.0 | 0.912 | -5.93 | 83.7% | 16.6% |
| throughput1m | nvme | 8 | 1M | 793.0 | 766.0 | 0.966 | -5.44 | 69.9% | 30.7% |
| throughput1m | hdd | 1 | 1M | 588.2 | 567.2 | 0.964 | -6.54 | 100.0% | 0.0% |
| throughput1m | hdd | 4 | 1M | 904.7 | 939.3 | 1.038 | -5.40 | 75.8% | 24.4% |
| throughput1m | hdd | 8 | 1M | 1268.0 | 1409.8 | 1.112 | -6.79 | 80.5% | 20.1% |

The `throughput512` profile has effective max buffer size `512K`. Its `1M` rows are software fallback by design; those rows are included in the CSVs to verify fallback behavior but are not listed above as QAT-engine results.

The media split matters. HDD showed consistent throughput-profile wins at concurrent `512K`, and `1M` improved on HDD at jobs `4` and `8`. NVMe was mixed: `512K` improved at jobs `4` and `8` under `throughput1m`, but `1M` was slower than software for all NVMe job counts in this run. This supports keeping rotational/flash profile bias on the follow-up list.

## Request-Overhead Summary

| Profile | Media | Jobs | Record | QAT share | Setup us/req | Submit us/req | Wait ms/MiB | Wait ms/req | Src bufs | Dst bufs | Async max |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| default | nvme | 1 | 128K | 100.0% | 9.2 | 10.3 | 6.3 | 0.8 | 32 | 37 | 0 |
| default | nvme | 4 | 128K | 100.0% | 9.6 | 10.5 | 10.9 | 1.4 | 32 | 37 | 0 |
| default | nvme | 8 | 128K | 100.0% | 12.3 | 12.3 | 12.0 | 1.5 | 32 | 37 | 0 |
| default | hdd | 1 | 128K | 100.0% | 6.1 | 8.1 | 5.9 | 0.7 | 32 | 37 | 0 |
| default | hdd | 4 | 128K | 100.0% | 6.1 | 8.2 | 10.7 | 1.3 | 32 | 37 | 0 |
| default | hdd | 8 | 128K | 100.0% | 6.5 | 8.6 | 10.3 | 1.3 | 32 | 37 | 0 |
| throughput512 | nvme | 4 | 512K | 45.6% | 26.4 | 22.0 | 20.2 | 10.1 | 128 | 145 | 96 |
| throughput512 | nvme | 8 | 512K | 56.2% | 24.8 | 21.7 | 21.0 | 10.5 | 128 | 145 | 96 |
| throughput512 | hdd | 4 | 512K | 47.4% | 21.8 | 22.1 | 20.2 | 10.1 | 128 | 145 | 96 |
| throughput512 | hdd | 8 | 512K | 52.4% | 22.8 | 23.9 | 20.2 | 10.1 | 128 | 145 | 96 |
| throughput1m | nvme | 4 | 1M | 83.7% | 44.1 | 39.0 | 32.1 | 32.1 | 256 | 289 | 192 |
| throughput1m | nvme | 8 | 1M | 69.9% | 39.6 | 39.3 | 32.6 | 32.6 | 256 | 289 | 192 |
| throughput1m | hdd | 4 | 1M | 75.8% | 39.3 | 40.1 | 35.7 | 35.7 | 256 | 289 | 192 |
| throughput1m | hdd | 8 | 1M | 80.5% | 38.5 | 39.8 | 29.0 | 29.0 | 256 | 289 | 192 |

Local request setup/submit cost is measurable but is not the dominant latency term in these current-code rows:

- `128K` sync requests spend about `14` to `25 us/req` in setup plus submit, while QAT wait is about `0.7` to `1.5 ms/req`.
- `512K` async requests spend about `44` to `49 us/req` in setup plus submit, while QAT wait is about `10 ms/req` under jobs `4` and `8`.
- `1M` async requests spend about `78` to `84 us/req` in setup plus submit, while QAT wait is about `29` to `36 ms/req` under jobs `4` and `8`.

This does not mean request construction is irrelevant, but it means pure setup micro-optimizations must be low risk and validated end-to-end. The rejected contiguous-scratch experiment is consistent with this: reducing local setup cost can still regress whole-system elapsed time and QAT share.

## Conclusions

- The fresh baseline confirms that software fallback remains essential. Several whole-system wins are hybrid wins with substantial fallback, especially at `512K`.
- `512K` throughput-profile rows show useful elapsed wins on HDD at jobs `4` and `8`, and mixed but sometimes useful wins on NVMe.
- `1M` throughput-profile rows are useful on HDD at jobs `4` and `8`, but not on NVMe in this run.
- QAT mode consistently saves CPU active time when it actually offloads, even when elapsed time is not faster than software.
- Request setup/submit overhead is much smaller than QAT wait time at `512K` and `1M`; the next request-overhead work should avoid changing request shape unless it directly improves QAT share or wait behavior.

## Recommended Next Target

The next practical target is a narrow admission/profile policy experiment, not another scratch or buffer-list shape rewrite:

1. Add instrumentation or use existing counters to separate profile fallback from async cap fallback in the comparison summaries.
2. Test whether the `512K` throughput-profile cap should be media/profile biased, because HDD benefits were materially stronger than NVMe.
3. Keep any source change gated behind profile behavior so default balanced semantics remain predictable.
4. Treat local request-construction reductions as secondary unless a candidate can preserve request shape and QAT share.
