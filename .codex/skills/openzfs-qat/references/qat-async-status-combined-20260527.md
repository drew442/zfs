# QAT Async Status Combined Check Experiment - 2026-05-27

## Purpose

Test whether the suspended async ZIO path can reduce hot-path locking by reading
`complete` and `timed_out` state under one async-state spinlock acquisition.

The attempted change added a helper that returned both request-completion state
and timeout state in one call. The existing completion check after timeout and
abandon handling was retained so the race behavior around late completion was
not intentionally changed.

## Outcome

Rejected. The code compiled, installed, rebooted, and passed SHA correctness in
the benchmark, but the focused 1M benchmark regressed materially. No source
behavior from this experiment was kept.

The measured setup and submit counters show that collapsing this pair of status
checks is not a useful request-overhead target as implemented. Any saved
spinlock acquisition was below the level that matters to throughput and
latency, while elapsed time and write bandwidth both moved in the wrong
direction.

## Artifacts

- `artifacts/async-status-combined-20260527/zfs-qat-async-status-combined-1m-jobs4-20260527.csv`
- `artifacts/async-status-combined-20260527/zfs-qat-async-status-combined-1m-jobs8-20260527.csv`
- `artifacts/async-status-combined-20260527/summary.csv`

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

| Case | Jobs | Mean elapsed ms | Mean write MiB/s | CPU active s/GiB | QAT byte share | Setup ns/request | Submit ns/request |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| page lookup cache baseline | 4 | 776.716 | 941.027 | 5.277 | 84.347% | 73320.183 | 38749.953 |
| async status combined | 4 | 897.262 | 813.673 | 5.403 | 86.490% | 74022.075 | 40346.331 |
| delta | 4 | +15.520% | -13.533% | +2.384% | +2.143 pp | +0.957% | +4.120% |
| page lookup cache baseline | 8 | 989.293 | 1477.160 | 7.153 | 79.823% | 44507.855 | 41463.367 |
| async status combined | 8 | 1251.412 | 1166.577 | 6.738 | 78.180% | 40893.633 | 41052.432 |
| delta | 8 | +26.496% | -21.026% | -5.807% | -1.643 pp | -8.120% | -0.991% |

## Interpretation

Do not keep or repeat this optimization as implemented. It reduces one narrow
lock-reading pattern, but the dominant async cost remains request preparation,
QAT wait/service time, and cap/fallback behavior rather than the extra
completion-state lock in the suspended ZIO path.

The jobs=8 run did reduce CPU active seconds per GiB and setup timing, but both
throughput and elapsed latency are first-class requirements for this project.
The throughput regression is too large to accept for a CPU-only improvement.

Better next targets:

- Reduce request count or request preparation work before QAT submission.
- Investigate QAT driver-side queueing or response behavior where wait time is
  large enough to affect benchmark-level results.
- Keep rejected hot-path micro-optimizations documented so they are not
  repeatedly rediscovered without new evidence.

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
