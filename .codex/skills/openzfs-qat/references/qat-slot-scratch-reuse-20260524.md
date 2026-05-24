# QAT Slot Scratch Page-Array Reuse - 2026-05-24

## Goal

Continue request-overhead reduction after exact source/destination page counts by removing the remaining per-request scratch page-array heap allocation for large records, especially 1M records.

The previous page-count pass showed:

- 512K scratch page arrays fit the stack page-array path.
- 1M scratch page arrays still used a per-request heap allocation because the scratch page count exceeds `QAT_DC_STACK_MAX_PAGES`.

## Code Change

`module/os/linux/zfs/qat_compress.c` now allows an exclusive per-instance buffer slot to retain scratch page-array storage:

- `qat_dc_buffer_slot_t` has reusable `scratch_pages`, `scratch_pages_count`, and `scratch_pages_size` fields.
- `qat_dc_buffer_slot_scratch_pages()` lazily allocates or grows the page-array storage for that slot.
- Synchronous and asynchronous compression submit paths use slot scratch page arrays when:
  - scratch/additional destination pages are needed,
  - the count exceeds the stack threshold,
  - and a buffer slot was acquired.
- If no slot is available, the code keeps the existing per-request heap allocation and software fallback behavior.
- Slot cleanup frees retained scratch page-array storage when the buffer pool is cleaned.

Observability was updated:

- Added `dc_compress_page_array_slot_scratch`.
- Updated `qat-phase4-benchmark.sh` to capture `dc_compress_page_array_slot_scratch_delta`.

This keeps the memory lifetime bounded by the existing buffer-slot pool rather than adding a new global cache.

## Validation

Local checks:

- `git diff --check`
- `bash -n .codex/skills/openzfs-qat/scripts/qat-phase4-benchmark.sh`

Host checks on `pve.drewnet.online`:

- Synced source to `/usr/src/zfs-2.4.99`.
- Forced ZFS DKMS rebuild with `ICP_ROOT=/usr/src/qat-4.28.0-00004`.
- Installed the rebuilt module and updated initramfs.
- Rebooted into `7.0.0-3-pve`.
- Verified new loaded module `srcversion: 846D0E782C744511E774340`.
- Verified `dc_compress_page_array_slot_scratch` exists in `/proc/spl/kstat/zfs/qat`.
- Restored the host to the default safe profile after benchmarks.
- Verified `zpool status -x`: all pools healthy.

Benchmark profile:

- Temporary profile: `zfs_qat_dc_profile=throughput`
- Temporary profile record size: `zfs_qat_dc_profile_recordsize=1048576`
- Expected ratio: `unknown`
- Source: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`
- Pool/root: `nvme_scratch/bench`
- Record sizes: `512K 1M`
- Jobs: `4`, `8`
- Iterations: `3`
- Modes: `qat sw`
- Run order: `record`

## Artifacts

- `artifacts/slot-scratch-reuse-20260524/zfs-qat-slot-scratch-reuse-nvme-tiff-jobs4-20260524.csv`
- `artifacts/slot-scratch-reuse-20260524/zfs-qat-slot-scratch-reuse-nvme-tiff-jobs8-20260524.csv`
- `artifacts/slot-scratch-reuse-20260524/summary.csv`

The copied CSVs have a consistent 248-column width.

## Results

The direct structural result is clean: 1M scratch page arrays moved from per-request heap to slot reuse.

| record | jobs | heap scratch arrays before | slot scratch arrays after | page-array alloc ns/request before | page-array alloc ns/request after | page-array free ns/request before | page-array free ns/request after |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1M | 4 | 583.000 | 566.667 | 3311.221 | 2702.229 | 2045.134 | 1413.118 |
| 1M | 8 | 1123.667 | 1155.333 | 3270.563 | 2497.029 | 2202.201 | 1488.615 |

512K remains on the stack scratch page-array path, as expected:

| record | jobs | stack scratch arrays | heap scratch arrays | slot scratch arrays |
|---|---:|---:|---:|---:|
| 512K | 4 | 762.667 | 0.000 | 0.000 |
| 512K | 8 | 1673.333 | 0.000 | 0.000 |

Elapsed and CPU results are mixed, which is expected for a local allocation reduction under dominant QAT service latency:

| record | jobs | elapsed before ms | elapsed after ms | CPU before s/GiB | CPU after s/GiB | QAT byte share before | QAT byte share after |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1M | 4 | 746.772 | 738.335 | 5.245 | 5.286 | 79.870% | 77.630% |
| 1M | 8 | 899.433 | 909.651 | 5.798 | 5.728 | 76.967% | 79.140% |
| 512K | 4 | 734.939 | 742.696 | 6.969 | 7.367 | 52.240% | 52.243% |
| 512K | 8 | 968.727 | 861.216 | 9.332 | 7.528 | 52.870% | 57.313% |

## Interpretation

This is a useful request-overhead reduction because it removes a repeated heap allocation/free path for 1M scratch page arrays without changing QAT admission policy or reducing QAT share.

The performance signal should be read narrowly:

- Page-array allocation/free cost per request improved for 1M.
- 1M elapsed time was approximately flat within normal run variation.
- QAT wait/service time still dominates total elapsed time.
- Remaining page-array allocation/free time is now primarily source and destination page arrays, not scratch page arrays.

## Next Target

The next request-overhead target is source/destination page-array storage for large records:

- 1M requests still need heap-backed source and destination page arrays because they exceed `QAT_DC_STACK_MAX_PAGES`.
- Reusing those arrays through the same exclusive buffer-slot mechanism should reduce the remaining page-array allocation/free cost.
- This should be done carefully because source/destination page-array counts vary with coalescing and record size, but the ownership model is the same as scratch page-array reuse.
