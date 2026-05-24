# QAT Page-Count Request-Overhead Reduction - 2026-05-24

## Goal

Reduce per-request setup work in the ZFS QAT DC path without changing QAT admission policy, software fallback, compression level, async policy, or profile semantics.

The specific target was the conservative page-array sizing used for QAT buffer-list construction. The old path sized source, destination, and scratch page arrays with `(len >> PAGE_SHIFT) + 2` even when the actual virtual address and length were known. That kept the code safe, but overestimated known source and destination page counts and could force unnecessary heap page-array allocations or larger buffer-list allocations.

## Code Change

`module/os/linux/zfs/qat_compress.c` now has two page-count helpers:

- `qat_dc_page_count(ptr, len)` returns the exact number of pages touched by a known virtual address and length.
- `qat_dc_worst_page_count(len)` preserves the previous conservative count for scratch/additional destination space before the backing address is known.

The synchronous and asynchronous QAT DC submit paths now:

- Use exact page counts for known source and destination ranges.
- Keep worst-case counts for scratch/additional destination space when the address is not yet known.
- Use the synchronous stack scratch-page array when the required scratch page count fits `QAT_DC_STACK_MAX_PAGES`.
- Record synchronous scratch page-array stack/heap kstats according to the actual selected path.

This is intentionally a mechanical request-shaping change. It does not try to hide QAT service latency and does not increase the amount of data sent to QAT.

## Validation

Local checks:

- `git diff --check`
- `bash -n .codex/skills/openzfs-qat/scripts/qat-phase4-benchmark.sh`

Host checks on `pve.drewnet.online`:

- Synced source to `/usr/src/zfs-2.4.99`.
- Rebuilt and installed ZFS DKMS against `ICP_ROOT=/usr/src/qat-4.28.0-00004`.
- Ran `depmod`.
- Ran `update-initramfs -u -k 7.0.0-3-pve`.
- Rebooted into `7.0.0-3-pve`.
- Verified `zpool status -x`: all pools healthy.

Benchmark profile:

- Temporary profile: `zfs_qat_dc_profile=throughput`
- Temporary profile record size: `zfs_qat_dc_profile_recordsize=1048576`
- Expected ratio: `unknown`
- Source: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`
- Pool/root: `nvme_scratch/bench`
- Record sizes: `128K 512K 1M`
- Jobs: `4`, `8`
- Iterations: `3`
- Modes: `qat sw`
- Run order: `record`

The host was restored afterward to the default safe profile:

- `zfs_qat_dc_profile=balanced`
- `zfs_qat_dc_profile_recordsize=131072`
- `zfs_qat_dc_expected_ratio=unknown`
- `zpool status -x`: all pools healthy

## Artifacts

- `artifacts/page-count-overhead-20260524/zfs-qat-page-count-overhead-nvme-tiff-jobs4-20260524.csv`
- `artifacts/page-count-overhead-20260524/zfs-qat-page-count-overhead-nvme-tiff-jobs8-20260524.csv`
- `artifacts/page-count-overhead-20260524/summary.csv`

The copied CSVs have a consistent 247-column width, so the earlier decompression-counter CSV alignment concern is not present in this run.

## Results

The 128K rows are not useful for measuring QAT request overhead in this profile because the current throughput/1M profile routes 128K records to software. The useful QAT rows are 512K and 1M.

| record | jobs | elapsed ms | write MiB/s | CPU active s/GiB | QAT byte share | fallback share | setup ns/request | page-array alloc ns/request | scratch page-array path |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 512K | 4 | 734.939 | 993.597 | 6.969 | 52.240% | 47.760% | 34013.974 | 2103.938 | stack |
| 512K | 8 | 968.727 | 1531.560 | 9.332 | 52.870% | 47.277% | 39896.001 | 2894.750 | stack |
| 1M | 4 | 746.772 | 978.260 | 5.245 | 79.870% | 20.357% | 80244.207 | 3311.221 | heap |
| 1M | 8 | 899.433 | 1624.817 | 5.798 | 76.967% | 23.650% | 49515.535 | 3270.563 | heap |

Closest prior comparison is the post-scratch-reuse fullmatrix from 2026-05-23. This is not a strict isolated A/B because benchmark date and surrounding profile policy changed, but it is the nearest same-source/same-profile comparison in the repository.

| record | jobs | prior elapsed ms | current elapsed ms | prior CPU s/GiB | current CPU s/GiB | prior QAT byte share | current QAT byte share |
|---|---:|---:|---:|---:|---:|---:|---:|
| 512K | 4 | 906.589 | 734.939 | 7.676 | 6.969 | 47.650% | 52.240% |
| 512K | 8 | 1176.027 | 968.727 | 7.497 | 9.332 | 54.204% | 52.870% |
| 1M | 4 | 910.550 | 746.772 | 6.000 | 5.245 | 70.310% | 79.870% |
| 1M | 8 | 1283.778 | 899.433 | 5.975 | 5.798 | 76.542% | 76.967% |

## Interpretation

The implementation is safe and worth keeping because it removes conservative over-allocation from known page ranges and uses stack storage where possible. The strongest direct signal is structural rather than elapsed-time:

- 512K requests use stack scratch page arrays.
- 1M requests still require heap scratch page arrays because the scratch page count exceeds the stack-page threshold.
- Source and destination buffer counts are now sized from actual address ranges instead of a fixed `+2` estimate.

The elapsed-time comparison is favorable at 512K and 1M, but should not be treated as proof that this patch alone caused all of the measured improvement. QAT wait/service time remains the dominant cost; this change only reduces local setup overhead around each request.

## Next Target

Continue request-overhead reduction in areas that change per-request work without reducing useful QAT share:

- Recheck whether 1M scratch page-array heap allocation can be reduced safely without increasing memory lifetime too much.
- Investigate reusable or per-CPU page-array storage for request-local arrays that exceed the stack threshold.
- Keep 128K out of QAT in throughput-oriented profiles unless a later change materially improves small-record QAT latency.
