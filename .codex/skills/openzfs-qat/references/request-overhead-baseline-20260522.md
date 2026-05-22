# Request Overhead Baseline - 2026-05-22

Purpose: execute the first step 5 target from
`async-profile-matrix-20260521.md`: add request-shape observability, validate
the benchmark harness, and establish a narrow baseline before changing request
allocation or coalescing behavior.

## Implementation

Added QAT DC compression kstats for:

- Sync submit, completion, and fallback counts.
- Source, destination, and scratch page-array stack versus heap path counts.
- Page-array allocation and free time.
- Fallback QAT buffer-list allocation and free time.
- Per-request state allocation and free time.

Updated `qat-phase4-benchmark.sh` to include the new raw counters plus derived
per-request fields:

- `qat_setup_ns_per_req`
- `qat_submit_ns_per_req`
- `qat_wait_ns_per_req`
- `qat_cleanup_ns_per_req`
- `qat_page_array_alloc_ns_per_req`
- `qat_page_array_free_ns_per_req`
- `qat_buffer_list_alloc_ns_per_req`
- `qat_buffer_list_free_ns_per_req`
- `qat_req_alloc_ns_per_req`
- `qat_req_free_ns_per_req`
- `qat_src_buffers_per_req`
- `qat_dst_total_buffers_per_req`
- `qat_bound_bytes_per_req`
- `qat_scratch_bytes_per_req`
- `qat_sync_completion_share_pct`
- `qat_sync_fallback_share_pct`

The summary rows now use the CSV header as the source of truth so future column
additions do not recreate the earlier column-alignment risk.

## Validation Runs

Host: `pve.drewnet.online`

Module state:

```text
modinfo -n zfs -> /lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko
zfs srcversion -> E8FA567F53966DCF8F4259A
```

Smoke artifact:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-shape-smoke-128k-jobs1-20260522.csv
```

Narrow baseline artifacts:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-shape-sync-jobs4-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-shape-async-throughput-jobs4-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-shape-summary-20260522.csv
```

CSV validation:

```text
smoke: 9 rows, 220 columns, alignment OK
sync baseline: 21 rows, 220 columns, alignment OK
async throughput baseline: 21 rows, 220 columns, alignment OK
```

The host was temporarily booted with
`zfs_qat_dc_profile_recordsize=1048576` so `64K`, `128K`, `256K`, `512K`, and
`1M` could all be represented in the request-shape baseline. It was restored
afterward to:

```text
zfs_qat_dc_profile=balanced
zfs_qat_dc_profile_recordsize=131072
zfs_qat_dc_max_buf_size=profile
zfs_qat_dc_async=profile
zfs_qat_dc_poll=profile
zfs_qat_decompress_disable=profile
zfs_qat_compress_disable=0
dc_watchdog_health=1
dc_compress_quarantine_dst_retained=0
dc_compress_async_inflight=0
```

## Results

Narrow baseline shape: `JOBS=4`, `ITERS=1`, `VERIFY_MODE=sw`, records
`64K 128K 256K 512K 1M`.

Sync QAT, balanced profile:

| Record | QAT ms | SW ms | QAT vs SW | Wait ns/req | Setup ns/req | Submit ns/req | Cleanup ns/req | Page alloc ns/req | Req alloc ns/req | Src bufs/req | Dst bufs/req |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64K | 1393.9 | 1290.4 | +8.0% | 534482.8 | 4427.6 | 5443.5 | 734.0 | 385.9 | 238.4 | 16 | 19 |
| 128K | 1088.2 | 1134.5 | -4.1% | 1335675.3 | 6555.4 | 7640.8 | 765.9 | 305.1 | 217.9 | 32 | 37 |
| 256K | 1048.7 | 1078.8 | -2.8% | 2556910.8 | 11680.2 | 11705.0 | 1476.0 | 1089.6 | 185.4 | 64 | 73 |
| 512K | 1082.7 | 1072.0 | +1.0% | 5038850.5 | 20085.8 | 19652.3 | 1642.7 | 1333.1 | 259.6 | 128 | 145 |
| 1M | 981.3 | 1003.7 | -2.2% | 10522729.5 | 36384.6 | 34842.5 | 1671.1 | 1377.2 | 277.6 | 256 | 289 |

Async QAT, throughput profile:

| Record | QAT ms | SW ms | QAT vs SW | QAT byte % | Cap skip % | Wait ns/req | Setup ns/req | Submit ns/req | Cleanup ns/req | Page alloc ns/req | Req alloc ns/req |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64K | 1344.3 | 1401.8 | -4.1% | 0.0 | 100.0 | na | na | na | na | na | na |
| 128K | 1214.4 | 1095.0 | +10.9% | 100.0 | 0.0 | 9806174.5 | 17305.9 | 8643.5 | 3298.7 | 1392.7 | 648.5 |
| 256K | 1081.0 | 1094.4 | -1.2% | 47.9 | 52.1 | 19414907.4 | 69737.4 | 24533.2 | 7395.0 | 3451.3 | 1098.3 |
| 512K | 1029.1 | 999.3 | +3.0% | 52.3 | 47.7 | 12270592.3 | 60701.4 | 31132.5 | 9900.1 | 4239.2 | 928.2 |
| 1M | 908.2 | 911.0 | -0.3% | 85.5 | 14.8 | 31085679.4 | 109796.5 | 39186.4 | 5395.9 | 3762.9 | 727.5 |

## Interpretation

- ZFS-side request allocation is not the current dominant cost. Request
  allocation/free and page-array allocation/free are sub-microsecond to a few
  microseconds per QAT request in the measured rows.
- QAT wait time remains the dominant cost by a large margin. Sync wait grows
  from about `0.53 ms/request` at `64K` to about `10.5 ms/request` at `1M`.
- Async does not remove the wait cost; in this narrow run it often increases
  per-completed-QAT-request wait because the row is queue/admission shaped.
- Buffer-list fallback allocation time was zero in the measured rows because the
  existing per-instance buffer-slot pool handled the request shape.
- Source and destination scatter/gather shape scales linearly with record size:
  `64K` used about `16` source buffers and `19` destination buffers per request;
  `1M` used about `256` source buffers and `289` destination buffers per request.

## Decision

Do not start by optimizing small ZFS allocation paths. The new counters show
those paths are measurable but not large enough to explain the latency gap.

The next optimization target should be QAT service-time/request-shape behavior:

- Evaluate whether source coalescing should be profile-driven by record size,
  because scatter/gather entries scale directly with record size and QAT wait is
  the dominant cost.
- Re-test source coalescing with the new per-request counters before changing
  defaults. Prior source-coalescing results were mixed; the new counters can now
  show whether any win comes from lower QAT wait, lower setup cost, or just a
  policy/fallback change.
- Keep destination coalescing disabled by default until a repeat run shows a
  clear wait or elapsed-time benefit. It adds copy cost and previous results
  were mixed.
- Continue treating async wins as policy wins unless QAT byte share and
  wait-per-request improve at the same time.

## Follow-Up: Source Coalescing Retest

The immediate source-coalescing follow-up was completed on 2026-05-22; see
`source-coalescing-request-shape-20260522.md`.

Result:

- Source coalescing reliably reduced source scatter/gather shape to one source
  buffer per QAT request.
- The copy cost scaled with record size and was large enough to erase or reverse
  the benefit in most concurrent rows.
- Sync results were mixed and async results mostly regressed.
- No profile default was changed. `zfs_qat_dc_coalesce_src=profile` and
  `zfs_qat_dc_coalesce_dst=profile` should continue to resolve to off.

The next request-shape target should be alignment instrumentation and targeted
copying only for shapes that are proven harmful, not broad source coalescing.
