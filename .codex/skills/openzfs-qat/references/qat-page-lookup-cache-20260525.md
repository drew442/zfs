# QAT Page Lookup Cache - 2026-05-25

## Change

The QAT compression paths now classify source, destination, and scratch buffers
as vmalloc-backed or linear-backed once per request, then reuse that result for
each page segment in the buffer-list walk.

Before this change, each page segment called:

```text
qat_mem_to_page(data)
```

That helper checks `is_vmalloc_addr(data)` for every segment. At `1M`, the
current request shape is about `256` source buffers plus `289`
destination/scratch buffers per QAT request, so this address-space check scaled
directly with record size.

The new helper is:

```text
qat_mem_to_page_cached(data, is_vmalloc)
```

The sync and async compression paths set `src_vmalloc`, `dst_vmalloc`, and
`add_vmalloc` once after any source or destination coalescing decision. Coalesced
buffers stay on the existing linear `virt_to_page()` path.

## Validation

Local checks:

- `git diff --check`: pass
- `bash -n .codex/skills/openzfs-qat/scripts/qat-phase4-benchmark.sh`: pass

Host deployment:

- Synced source to `pve.drewnet.online:/usr/src/zfs-2.4.99/`
- Rebuilt and installed ZFS DKMS against `/usr/src/qat-4.28.0-00004`
- Ran `depmod -a` and `update-initramfs -u -k 7.0.0-3-pve`
- Rebooted into `7.0.0-3-pve`
- Verified `qat.service` active, pools healthy, no QAT failures, and
  `dc_watchdog_health=1`

CSV checks:

- `artifacts/page-lookup-cache-20260525/zfs-qat-page-lookup-cache-1m-jobs4-20260525.csv`: `254` fields per row
- `artifacts/page-lookup-cache-20260525/zfs-qat-page-lookup-cache-1m-jobs8-20260525.csv`: `254` fields per row

## Benchmark

Artifacts:

- `artifacts/page-lookup-cache-20260525/zfs-qat-page-lookup-cache-1m-jobs4-20260525.csv`
- `artifacts/page-lookup-cache-20260525/zfs-qat-page-lookup-cache-1m-jobs8-20260525.csv`
- `artifacts/page-lookup-cache-20260525/summary.csv`

Comparison baseline:

- `artifacts/shape-stats-gating-20260525/zfs-qat-shape-stats-off-rerun-1m-jobs4-20260525.csv`
- `artifacts/shape-stats-gating-20260525/zfs-qat-shape-stats-off-rerun-1m-jobs8-20260525.csv`

Benchmark shape:

- Pool: `nvme_scratch`
- Source: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`
- Record size: `1M`
- Jobs: `4`, `8`
- Iterations: `3`
- Mode: `qat`
- Verify mode: `sw`
- Temporary boot profile: `zfs_qat_dc_profile=throughput`,
  `zfs_qat_dc_profile_recordsize=1048576`, `zfs_qat_dc_async=1`,
  `zfs_qat_dc_async_max_inflight=96`,
  `zfs_qat_dc_async_cap_policy=throughput`

## Results

| Case | Jobs | Mean elapsed ms | Mean write MiB/s | CPU active s/GiB | QAT byte share | Setup ns/request | Submit ns/request | Request alloc ns/request |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| shape-stats off baseline | 4 | 784.010 | 932.270 | 5.093 | 87.590% | 95352.376 | 38000.947 | 1091.359 |
| cached page lookup | 4 | 776.716 | 941.027 | 5.277 | 84.347% | 73320.183 | 38749.953 | 1165.680 |
| delta | 4 | -0.930% | +0.939% | +3.623% | -3.243 pp | -23.106% | +1.971% | +6.810% |
| shape-stats off baseline | 8 | 966.101 | 1511.433 | 6.663 | 81.310% | 47436.981 | 39825.385 | 1101.949 |
| cached page lookup | 8 | 989.293 | 1477.160 | 7.153 | 79.823% | 44507.855 | 41463.367 | 1395.404 |
| delta | 8 | +2.401% | -2.268% | +7.360% | -1.487 pp | -6.175% | +4.113% | +26.631% |

## Interpretation

This is a valid structural request-overhead reduction: it removes repeated
`is_vmalloc_addr()` checks from the per-page segment loop without changing
request admission policy, request shape, QAT instance selection, fallback
behavior, or compression output.

The benchmark does not prove an end-to-end throughput win. Jobs=4 was
effectively flat/slightly better, jobs=8 regressed slightly, and QAT byte share
changed between comparison runs. The setup timing moved in the expected
direction, but QAT wait/service time and fallback share still dominate the
observable elapsed-time result.

Keep the change because it is narrow, compiled cleanly, passed runtime
validation, and reduces per-request CPU work for large records. Do not use this
benchmark as evidence that request-overhead work has materially closed the gap
to software gzip by itself.

## Next Target

Continue request-overhead reduction only where the change can affect larger
cost centers than local nanosecond-scale setup work:

- Reduce request count or source/destination segment count where possible.
- Avoid fallback/cap-skip churn without lowering useful QAT byte share.
- Investigate whether larger records can be split or staged in a way that
  reduces QAT wait exposure without increasing CPU copy cost.
- Keep 1M in every benchmark matrix because large records expose the highest
  segment-walk and QAT wait cost.
