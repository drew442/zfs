# QAT Linear Kmap Bypass Experiment - 2026-05-26

## Purpose

Test whether QAT request setup cost can be reduced by bypassing `kmap()` and
page-array cleanup for non-vmalloc source, destination, and scratch buffers.

The attempted fast path kept the existing page-sized flat-buffer segmentation
but, for non-vmalloc buffers, used the existing kernel address directly as
`CpaFlatBuffer.pData` and skipped:

- `virt_to_page()` / `vmalloc_to_page()` lookup for that segment
- `kmap(page)`
- storing the page pointer for later cleanup
- `kunmap(page)`
- page-array allocation when all pages for that buffer class were non-vmalloc

The vmalloc path was intentionally left on the existing page lookup plus
`kmap()`/`kunmap()` behavior.

## Outcome

Rejected. The code compiled and passed correctness verification, but the 1M
benchmark regressed materially. The experiment was reverted and was not kept in
the source tree.

The host was restored to the prior committed module after the test:

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

## Artifacts

- `artifacts/linear-kmap-bypass-20260526/zfs-qat-linear-kmap-bypass-1m-jobs4-20260526.csv`
- `artifacts/linear-kmap-bypass-20260526/zfs-qat-linear-kmap-bypass-1m-jobs8-20260526.csv`
- `artifacts/linear-kmap-bypass-20260526/summary.csv`
- `artifacts/linear-kmap-bypass-yolo-rerun-20260527/zfs-qat-linear-kmap-bypass-yolo-rerun-1m-jobs4-20260527.csv`
- `artifacts/linear-kmap-bypass-yolo-rerun-20260527/zfs-qat-linear-kmap-bypass-yolo-rerun-1m-jobs8-20260527.csv`
- `artifacts/linear-kmap-bypass-yolo-rerun-20260527/summary.csv`

Comparison baseline:

- `artifacts/page-lookup-cache-20260525/zfs-qat-page-lookup-cache-1m-jobs4-20260525.csv`
- `artifacts/page-lookup-cache-20260525/zfs-qat-page-lookup-cache-1m-jobs8-20260525.csv`

CSV validation:

- jobs=4 artifact: `254` fields per row
- jobs=8 artifact: `254` fields per row
- YOLO rerun jobs=4 artifact: `254` fields per row
- YOLO rerun jobs=8 artifact: `254` fields per row

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

| Case | Jobs | Mean elapsed ms | Mean write MiB/s | CPU active s/GiB | QAT byte share | Setup ns/request | Submit ns/request | Request alloc ns/request |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| page lookup cache baseline | 4 | 776.716 | 941.027 | 5.277 | 84.347% | 73320.183 | 38749.953 | 1165.680 |
| linear kmap bypass | 4 | 925.441 | 789.987 | 5.318 | 86.443% | 61140.404 | 40695.456 | 1173.807 |
| delta | 4 | +19.148% | -16.051% | +0.779% | +2.097 pp | -16.612% | +5.021% | +0.697% |
| page lookup cache baseline | 8 | 989.293 | 1477.160 | 7.153 | 79.823% | 44507.855 | 41463.367 | 1395.404 |
| linear kmap bypass | 8 | 1569.092 | 941.410 | 8.103 | 81.013% | 60267.978 | 55596.652 | 2225.448 |
| delta | 8 | +58.607% | -36.269% | +13.278% | +1.190 pp | +35.410% | +34.086% | +59.484% |

## YOLO Rerun - 2026-05-27

The experiment was rerun after confirming the agent environment was operating
with unrestricted filesystem access and no approval prompts. This matters only
for local command execution; it should not change the host-side DKMS build,
kernel module behavior, or benchmark output. The rerun was still useful as a
control because the prior session had local sandbox friction.

Rerun host state before benchmark:

```text
zfs srcversion: 4BE8EE5B380B4BD19CD1A76
zfs_qat_dc_profile=throughput
zfs_qat_dc_profile_recordsize=1048576
zfs_qat_dc_async=1
zfs_qat_dc_async_max_inflight=96
zfs_qat_dc_async_cap_policy=throughput
zfs_qat_dc_shape_stats=profile
dc_fails=0
dc_instances=12
dc_watchdog_health=1
```

Rerun result:

| Case | Jobs | Mean elapsed ms | Mean write MiB/s | CPU active s/GiB | QAT byte share | Setup ns/request |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| page lookup cache baseline | 4 | 776.716 | 941.027 | 5.277 | 84.347% | 73320.183 |
| YOLO rerun linear kmap bypass | 4 | 951.211 | 771.303 | 5.468 | 77.910% | 77495.763 |
| delta | 4 | +22.466% | -18.036% | +3.611% | -6.437 pp | +5.695% |
| page lookup cache baseline | 8 | 989.293 | 1477.160 | 7.153 | 79.823% | 44507.855 |
| YOLO rerun linear kmap bypass | 8 | 1283.450 | 1137.610 | 6.054 | 81.057% | 41243.026 |
| delta | 8 | +29.734% | -22.987% | -15.370% | +1.233 pp | -7.335% |

The rerun confirms the original conclusion. The jobs=8 regression was smaller
than the first attempt but still material, and jobs=4 regressed again. The
result is not a sandbox artifact.

The host was restored again after the rerun:

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

## Interpretation

Do not repeat this optimization as implemented. It is tempting because the
large-record request shape has hundreds of flat buffers, but replacing
`kmap(page) + page_off` with the original linear buffer address did not improve
the result. The jobs=8 run showed a large elapsed-time and write-throughput
regression even though QAT byte share increased slightly.

The most likely conclusion is practical rather than theoretical: the existing
`kmap()` path should be treated as part of the known-good QAT buffer-address
contract for now. It may produce addresses or driver translation behavior that
the out-of-tree QAT 1.x stack handles better than direct original buffer
addresses, or the attempted fast path may interact poorly with address
translation in a way not visible from ZFS-side counters.

Because the result regressed at the benchmark level, the safe decision is to
leave `kmap()`/`kunmap()` intact and move to a different request-overhead target.

## Next Target

Avoid more local per-segment micro-optimizations unless they can be proven
without changing the QAT buffer-address contract. Better next targets:

- Add targeted instrumentation for buffer address class and mapping cost before
  changing mapping behavior again.
- Investigate request count and cap/fallback churn rather than per-segment
  nanosecond-scale local setup work.
- Investigate whether a QAT driver-side address-translation or batch-submit path
  exists for QAT 1.x and can be changed safely in the lock-step QAT fork.
- Keep 1M in all benchmark matrices because this record size exposes the
  highest page-segment and wait-time cost.
