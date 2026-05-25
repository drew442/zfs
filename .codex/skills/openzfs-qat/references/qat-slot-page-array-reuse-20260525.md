# QAT Slot Source/Destination Page-Array Reuse - 2026-05-25

## Goal

Continue request-overhead reduction after scratch page-array reuse by removing repeated heap allocation/free for source and destination page arrays on large records.

The previous slot scratch pass showed that page-array allocation/free time was still present after 1M scratch arrays moved to slot reuse. The remaining source was primarily source and destination page arrays.

## Code Change

`module/os/linux/zfs/qat_compress.c` now allows an exclusive per-instance buffer slot to retain source and destination page-array storage:

- `qat_dc_buffer_slot_t` has reusable `in_pages` and `out_pages` storage with count and size metadata.
- `qat_dc_buffer_slot_page_array()` is the common lazy allocate/grow helper for source, destination, and scratch page arrays.
- Synchronous and asynchronous compression submit paths use slot source/destination page arrays when:
  - the page count exceeds `QAT_DC_STACK_MAX_PAGES`,
  - and a buffer slot was acquired.
- If no slot is available, the existing per-request heap allocation path remains.
- Slot cleanup frees retained source, destination, and scratch page-array storage when the buffer pool is cleaned.

Observability was updated:

- Added `dc_compress_page_array_slot_src`.
- Added `dc_compress_page_array_slot_dst`.
- Updated `qat-phase4-benchmark.sh` to capture both new counters.

## Validation

Local checks:

- `git diff --check`
- `bash -n .codex/skills/openzfs-qat/scripts/qat-phase4-benchmark.sh`

Host checks on `pve.drewnet.online`:

- Synced source to `/usr/src/zfs-2.4.99`.
- Forced ZFS DKMS rebuild with `ICP_ROOT=/usr/src/qat-4.28.0-00004`.
- Installed the rebuilt module and updated initramfs.
- Rebooted into `7.0.0-3-pve`.
- Verified new loaded module `srcversion: 1E0E6166D789C38BEBB0A04`.
- Verified `dc_compress_page_array_slot_src`, `dc_compress_page_array_slot_dst`, and `dc_compress_page_array_slot_scratch` exist in `/proc/spl/kstat/zfs/qat`.
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

- `artifacts/slot-page-arrays-20260525/zfs-qat-slot-page-arrays-nvme-tiff-jobs4-20260525.csv`
- `artifacts/slot-page-arrays-20260525/zfs-qat-slot-page-arrays-nvme-tiff-jobs8-20260525.csv`
- `artifacts/slot-page-arrays-20260525/summary.csv`

The copied CSVs have a consistent 250-column width.

## Results

The structural result is clean: source and destination page arrays moved from heap to slot reuse for both 512K and 1M rows.

| record | jobs | slot src arrays | heap src arrays | slot dst arrays | heap dst arrays | slot scratch arrays |
|---|---:|---:|---:|---:|---:|---:|
| 512K | 4 | 771.667 | 0.000 | 771.667 | 0.000 | 0.000 |
| 512K | 8 | 1615.000 | 0.000 | 1615.000 | 0.000 | 0.000 |
| 1M | 4 | 615.000 | 0.000 | 615.000 | 0.000 | 615.000 |
| 1M | 8 | 1108.000 | 0.000 | 1108.000 | 0.000 | 1108.000 |

Page-array allocation/free cost per QAT request dropped materially compared with the prior slot-scratch run:

| record | jobs | alloc ns/request before | alloc ns/request after | free ns/request before | free ns/request after |
|---|---:|---:|---:|---:|---:|
| 512K | 4 | 2166.015 | 271.097 | 1373.042 | 0.000 |
| 512K | 8 | 2344.652 | 280.326 | 1511.130 | 0.000 |
| 1M | 4 | 2702.229 | 756.827 | 1413.118 | 99.941 |
| 1M | 8 | 2497.029 | 212.798 | 1488.615 | 0.000 |

Elapsed and CPU results remain mixed under dominant QAT wait/service time:

| record | jobs | elapsed before ms | elapsed after ms | CPU before s/GiB | CPU after s/GiB | QAT byte share before | QAT byte share after |
|---|---:|---:|---:|---:|---:|---:|---:|
| 512K | 4 | 742.696 | 732.371 | 7.367 | 6.983 | 52.243% | 52.860% |
| 512K | 8 | 861.216 | 927.108 | 7.528 | 8.164 | 57.313% | 55.313% |
| 1M | 4 | 738.335 | 788.525 | 5.286 | 5.164 | 77.630% | 84.257% |
| 1M | 8 | 909.651 | 821.672 | 5.728 | 5.637 | 79.140% | 75.897% |

## Interpretation

This is a successful request-overhead reduction. It removes almost all page-array free cost and most page-array allocation cost for the tested large-record QAT paths without lowering QAT byte share or changing fallback policy.

The elapsed-time results should not be over-interpreted:

- 512K/jobs4 and 1M/jobs8 improved.
- 512K/jobs8 and 1M/jobs4 regressed.
- QAT service/wait time remains one to two orders of magnitude larger than page-array setup time.
- The local setup reduction is still worthwhile because it removes per-request allocator churn from every useful QAT request.

## Next Target

The request-overhead work has removed the obvious page-array heap churn. The next practical target is per-request `qat_dc_sync_req_t` allocation in the synchronous path:

- Async already embeds request state in `qat_dc_async_t`.
- Sync compression still allocates and frees `qat_dc_sync_req_t` per request.
- A small reusable sync request object in the exclusive buffer slot may reduce another allocator path.
- This needs careful handling for timeout/quarantine retention, because retained sync requests cannot be reused until the late-completion cleanup releases them.
