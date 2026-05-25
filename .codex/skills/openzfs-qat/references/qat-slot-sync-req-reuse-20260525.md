# QAT Slot Sync Request Reuse - 2026-05-25

## Goal

Continue request-overhead reduction after page-array reuse by reducing synchronous request-object allocation/free churn.

The target is `qat_dc_sync_req_t` in the synchronous QAT compression path. Async compression embeds request state in `qat_dc_async_t`; sync compression previously allocated and freed a `qat_dc_sync_req_t` per request.

## Code Change

`module/os/linux/zfs/qat_compress.c` now allows an exclusive per-instance buffer slot to retain a reusable sync request object:

- `qat_dc_buffer_slot_t` has a `sync_req` pointer.
- `qat_dc_buffer_slot_sync_req()` lazily allocates a slot-owned sync request and prepares it for each use.
- `qat_dc_sync_req_prepare()` centralizes request initialization for slot-owned and per-request sync requests.
- `qat_dc_sync_req_t` has a `from_slot` marker so retained-request cleanup can distinguish slot-owned objects from heap-owned objects.
- Normal completion reuses the slot-owned object and avoids per-request free.
- If no buffer slot is available, the existing per-request allocation/free path remains.

The watchdog/quarantine retained path remains safe:

- If a recoverable timeout retains a request, the buffer slot remains busy.
- The late QAT callback calls retained cleanup.
- Retained cleanup releases the slot but does not free a slot-owned `sync_req`.
- The slot cannot be reused while its retained request can still receive a late callback.

Observability was updated:

- Added `dc_compress_req_slot`.
- Updated `qat-phase4-benchmark.sh` to capture `dc_compress_req_slot_delta`.

## Validation

Local checks:

- `git diff --check`
- `bash -n .codex/skills/openzfs-qat/scripts/qat-phase4-benchmark.sh`

Host checks on `pve.drewnet.online`:

- Synced source to `/usr/src/zfs-2.4.99`.
- Forced ZFS DKMS rebuild with `ICP_ROOT=/usr/src/qat-4.28.0-00004`.
- Installed the rebuilt module and updated initramfs.
- Rebooted into `7.0.0-3-pve`.
- Verified new loaded module `srcversion: 94E51A84CB58D78BC6817D8`.
- Verified `dc_compress_req_slot` exists in `/proc/spl/kstat/zfs/qat`.
- Restored the host to the default safe profile after benchmarks.
- Verified `zpool status -x`: all pools healthy.

Build note:

- The first DKMS build failed because `qat_dc_buffer_slot_t` referenced `qat_dc_sync_req_t` before its forward typedef.
- The typedef was moved before the buffer-slot struct and the forced DKMS rebuild then succeeded.

Benchmark profile:

- Temporary profile: `zfs_qat_dc_profile=throughput`
- Temporary profile record size: `zfs_qat_dc_profile_recordsize=1048576`
- Async explicitly disabled: `zfs_qat_dc_async=0`
- Expected ratio: `unknown`
- Source: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`
- Pool/root: `nvme_scratch/bench`
- Record sizes: `512K 1M`
- Jobs: `4`, `8`
- Iterations: `3`
- Modes: `qat sw`
- Run order: `record`

## Artifacts

- `artifacts/slot-sync-req-20260525/zfs-qat-slot-sync-req-nvme-tiff-jobs4-20260525.csv`
- `artifacts/slot-sync-req-20260525/zfs-qat-slot-sync-req-nvme-tiff-jobs8-20260525.csv`
- `artifacts/slot-sync-req-20260525/summary.csv`

The copied CSVs have a consistent 251-column width.

## Metric Correction

The first version of this note reported `qat_byte_share_pct` values above 100%.
That was not a valid byte share. The raw CSVs were produced by the older
benchmark harness, where `qat_byte_share_pct` was the uncapped
`comp_in_delta / source_bytes` ratio.

The underlying counters are still useful: `comp_in_delta` can slightly exceed
the exact copied file bytes because the QAT counter tracks ZFS compression input
at record/block granularity. For this summary, `qat_byte_share_pct` is capped at
100%, and the uncapped diagnostic value is shown separately as
`qat_input_to_source_pct`.

## Results

The benchmark intentionally disables async so the synchronous path is exercised. All QAT rows show `zfs_qat_dc_effective_async=0`, zero async submits, and matching sync submit/completion counts.

| record | jobs | sync submits | sync completions | async submits | request slot uses | request free ns/request |
|---|---:|---:|---:|---:|---:|---:|
| 512K | 4 | 1460.000 | 1460.000 | 0.000 | 1460.000 | 0.000 |
| 512K | 8 | 2927.333 | 2927.333 | 0.000 | 2927.333 | 0.000 |
| 1M | 4 | 732.000 | 732.000 | 0.000 | 732.000 | 0.000 |
| 1M | 8 | 1471.000 | 1471.000 | 0.000 | 1471.000 | 0.000 |

Request allocation/free timing:

| record | jobs | request alloc ns/request | request free ns/request | setup ns/request | wait ns/MiB |
|---|---:|---:|---:|---:|---:|
| 512K | 4 | 442.915 | 0.000 | 28695.799 | 10006904.667 |
| 512K | 8 | 537.781 | 0.000 | 30064.399 | 10873841.667 |
| 1M | 4 | 450.314 | 0.000 | 134364.550 | 10744334.667 |
| 1M | 8 | 677.788 | 0.000 | 50290.347 | 10498366.333 |

Throughput and CPU:

| record | jobs | elapsed ms | write MiB/s | CPU s/GiB | QAT byte share | QAT input/source |
|---|---:|---:|---:|---:|---:|---:|
| 512K | 4 | 844.567 | 864.687 | 3.301 | 100.000% | 100.010% |
| 512K | 8 | 989.623 | 1475.217 | 4.024 | 100.000% | 100.260% |
| 1M | 4 | 854.079 | 854.667 | 3.637 | 100.000% | 100.280% |
| 1M | 8 | 969.484 | 1505.877 | 3.897 | 100.000% | 100.760% |

## Interpretation

This is a successful structural request-overhead reduction for the synchronous path:

- Every measured sync QAT request used the slot-owned request object.
- Per-request sync request free time is eliminated in the measured rows.
- Allocation timing is now mostly the cost of preparing or first allocating slot-owned request state, not a guaranteed per-request heap allocate/free pair.

The elapsed-time result should not be compared directly with the prior async throughput matrix:

- This benchmark intentionally disables async.
- QAT byte share is 100% in the corrected bounded metric, while prior async
  throughput profiles allowed fallback under pressure.
- QAT wait/service time still dominates the local request-object cost.

## Next Target

The obvious allocator churn in the synchronous request path has been reduced. The next practical target is likely buffer-list metadata and destination/coalescing policy interactions under sync and async separately:

- For sync, confirm whether remaining setup time is mostly QAT bound/header work, buffer-list metadata, or destination scratch handling.
- For async, avoid optimizing sync-only paths unless profiles will use sync intentionally.
- Keep benchmark matrices separated by `zfs_qat_dc_async` because sync and async have materially different behavior.
