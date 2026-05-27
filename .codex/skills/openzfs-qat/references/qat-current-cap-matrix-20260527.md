# QAT Current-Code 1M Async Cap Matrix - 2026-05-27

## Purpose

Validate whether the retained current-code async cap policy should be changed after the rejected contiguous scratch experiment. This was a current-code policy check only; no source behavior was changed.

The test specifically checked 1M records because 1M must remain in benchmark coverage and because the throughput cap policy has a different scaling path for large records on hosts with more than six initialized DC instances.

## Host State

- Host: `pve.drewnet.online`
- Kernel: `7.0.0-3-pve`
- ZFS module `srcversion`: `4F680B7A990EAE933F81B16`
- QAT devices: 2x DH895XCC
- Active ZFS QAT DC instances: 12
- QAT service: active
- Pools: healthy
- Post-run restored boot profile:
  - `zfs_qat_dc_profile=balanced`
  - `zfs_qat_dc_profile_recordsize=131072`
  - `zfs_qat_dc_async=profile`
  - `zfs_qat_dc_async_max_inflight=profile`
  - `zfs_qat_dc_async_cap_policy=profile`
  - `zfs_qat_dc_shape_stats=profile`

## Test Shape

Artifacts are stored in `artifacts/current-cap-matrix-20260527/`.

Each file contains three raw iterations plus the harness summary row. The local `summary.csv` in the artifact directory was generated from the raw rows so that QAT share, fallback share, CPU, and timing metrics are averaged consistently.

Common settings:

- `RECORDS=1M`
- `MODES=qat`
- `ITERS=3`
- `VERIFY_MODE=sw`
- `RUN_ORDER=record`
- `JOBS=4` and `JOBS=8`

Cap settings tested:

- Fixed cap `96`
- Fixed cap `160`
- Fixed cap `192`
- Profile throughput policy with `zfs_qat_dc_async_max_inflight=96` and `zfs_qat_dc_async_cap_policy=throughput`

Important interpretation point: the throughput-policy rows still report `zfs_qat_dc_effective_async_max_inflight=96`, but the observed `dc_compress_async_inflight_max` reached `192`. That is expected because `qat_dc_async_effective_cap()` starts from the parameter value and then `qat_dc_async_recordsize_cap()` applies the throughput record-size policy. For 1M records in throughput mode, the code uses all initialized DC instances with a per-instance cap of 16. On this 12-DC-instance host, that produces an effective runtime cap of 192.

## Results

| Cap setting | Jobs | Elapsed mean ms | Write MiB/s mean | CPU active s/GiB | QAT byte share | Fallback share | QAT wait ms/MiB | Setup us/req | Observed inflight max | SHA |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| fixed 96 | 4 | 907.189 | 808.063 | 7.555 | 58.680% | 41.483% | 19.912 | 68.386 | 96 | yes |
| fixed 96 | 8 | 1325.071 | 1102.500 | 7.541 | 72.563% | 27.967% | 17.342 | 45.243 | 96 | yes |
| fixed 160 | 4 | 879.377 | 830.760 | 6.437 | 73.020% | 27.183% | 31.950 | 41.024 | 160 | yes |
| fixed 160 | 8 | 1497.055 | 982.800 | 7.943 | 79.823% | 20.797% | 22.573 | 44.174 | 160 | yes |
| fixed 192 | 4 | 970.292 | 753.320 | 5.148 | 89.277% | 10.973% | 32.672 | 39.104 | 192 | yes |
| fixed 192 | 8 | 1344.876 | 1087.243 | 6.447 | 82.200% | 18.480% | 27.030 | 39.422 | 192 | yes |
| throughput policy | 4 | 925.118 | 789.200 | 5.245 | 88.043% | 12.203% | 34.380 | 38.455 | 192 | yes |
| throughput policy | 8 | 1378.616 | 1064.780 | 7.115 | 79.913% | 20.740% | 27.626 | 42.445 | 192 | yes |

## Interpretation

The matrix does not support a source change.

- Fixed cap `160` had the best jobs4 elapsed mean, but jobs8 regressed materially and had the worst elapsed mean in this run.
- Fixed cap `96` had the best jobs8 elapsed mean, but it did so with much lower QAT byte share and higher fallback share. That is a hybrid fallback win, not evidence that QAT engine performance improved.
- Fixed cap `192` and the throughput policy maintained the highest QAT share, but they were not the fastest elapsed cases in this run.
- The throughput policy behaved as designed for this host: with 12 active DC instances and 1M records, it drove the runtime cap to 192.

Keep the current policy unchanged for now. The current policy remains a defensible throughput/offload setting because it preserves high QAT utilization for large records, while the faster elapsed cases in this matrix depend more heavily on software fallback or are not stable across job counts.

## Follow-Up

Continue request-overhead reduction in the QAT request path rather than retuning the 1M throughput cap from this matrix alone. If cap policy is revisited later, use paired baseline/current comparisons in the same host state and evaluate both QAT engine metrics and whole-system hybrid metrics.
