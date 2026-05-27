# QAT Async Precheck Experiment - 2026-05-27

## Purpose

Test whether async cap-skip fallback can avoid unnecessary request setup work.

The ZIO async gzip path allocates async state, allocates a destination ABD,
borrows/copies the source ABD buffer, borrows the destination buffer, and then
calls `qat_dc_compress_async_submit()`. If the async cap is already full,
`qat_dc_compress_async_submit()` returns `NULL` and ZIO falls back to software
after paying those setup costs.

Two variants were tested:

- Reserving pre-admission: reserve an async inflight slot before ZIO allocates
  or copies buffers, then submit through a pre-admitted path.
- Non-reserving precheck: check whether the cap is already visibly full before
  ZIO allocates or copies buffers, but leave actual inflight reservation in the
  normal submit path.

## Outcome

Rejected. Both variants compiled and passed correctness, but both regressed the
focused 1M benchmark. No source behavior from this experiment was kept.

The reserving design was rejected immediately because it changes the meaning of
the async inflight cap: the cap starts covering CPU-side ABD allocation/copy
time before hardware submission, which reduces effective QAT concurrency.

The non-reserving precheck was safer structurally, but still did not improve the
measured result. It avoids some cap-full setup work, but it adds another cap
check to every async candidate and did not produce a net win in the 1M run.

## Artifacts

- `artifacts/async-precheck-20260527/zfs-qat-async-precheck-1m-jobs4-20260527.csv`
- `artifacts/async-precheck-20260527/zfs-qat-async-precheck-1m-jobs8-20260527.csv`
- `artifacts/async-precheck-20260527/summary.csv`

Comparison baseline:

- `artifacts/page-lookup-cache-20260525/zfs-qat-page-lookup-cache-1m-jobs4-20260525.csv`
- `artifacts/page-lookup-cache-20260525/zfs-qat-page-lookup-cache-1m-jobs8-20260525.csv`

CSV validation:

- jobs=4 artifact: `254` fields per row
- jobs=8 artifact: `254` fields per row

## Benchmark Shape

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

| Case | Jobs | Mean elapsed ms | Mean write MiB/s | CPU active s/GiB | QAT byte share | Setup ns/request |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| page lookup cache baseline | 4 | 776.716 | 941.027 | 5.277 | 84.347% | 73320.183 |
| async precheck | 4 | 872.797 | 838.323 | 5.564 | 76.033% | 58096.348 |
| delta | 4 | +12.370% | -10.914% | +5.428% | -8.313 pp | -20.765% |
| page lookup cache baseline | 8 | 989.293 | 1477.160 | 7.153 | 79.823% | 44507.855 |
| async precheck | 8 | 1302.415 | 1120.927 | 6.127 | 79.893% | 44669.098 |
| delta | 8 | +31.651% | -24.116% | -14.351% | +0.070 pp | +0.362% |

## Interpretation

Do not keep async cap pre-admission or the tested non-reserving precheck.

The reserving design is conceptually wrong for this cap because it includes
pre-submit CPU preparation time in the hardware inflight budget. The
non-reserving design is conceptually safer, but the measured benefit was not
present. It reduced setup timing in jobs=4, but elapsed time, write bandwidth,
and QAT byte share all moved the wrong way. Jobs=8 also regressed materially.

The useful lesson is that cap/fallback churn is not free, but avoiding it needs
to happen without adding checks to the hot path or changing the timing window of
the async cap. Future work should either:

- Improve the cap policy itself so fewer requests arrive at the cap boundary.
- Add lower-cost observability for cap pressure before attempting another
  admission change.
- Move to QAT driver-side queueing/response behavior, where the dominant wait
  cost is more likely to be affected.

## Host Restore

After the experiment, `pve.drewnet.online` was restored to the committed module
and default boot profile:

```text
zfs srcversion: 4F680B7A990EAE933F81B16
zfs_qat_dc_profile=balanced
zfs_qat_dc_profile_recordsize=131072
zfs_qat_dc_async=profile
zfs_qat_dc_async_max_inflight=profile
zfs_qat_dc_async_cap_policy=profile
zfs_qat_dc_shape_stats=profile
dc_fails=0
dc_instances=12
dc_watchdog_health=1
```
