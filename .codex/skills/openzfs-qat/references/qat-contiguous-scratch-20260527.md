# QAT Contiguous Scratch Buffer Experiment - 2026-05-27

## Purpose

Test whether QAT compression request overhead can be reduced by representing the
additional deflate-bound scratch output as one QAT-contiguous flat buffer.

At `1M`, the retained fragmented path builds about `256` source buffers and
`289` destination-side buffers per completed QAT request. Roughly `65` of those
destination-side buffers are scratch/additional output buffers. This experiment
kept source handling and the final destination ABD unchanged, but allocated the
scratch buffer with QAT contiguous memory and added it to the destination
buffer list as a single flat buffer.

The goal was to reduce destination-side SGL/request work without using full
destination coalescing, which adds a copy-back of the compressed result.

## Outcome

Rejected. The change compiled, installed, booted, and passed SHA correctness,
and it reduced destination-side flat-buffer shape as intended. End-to-end
throughput and elapsed latency regressed materially, so no source behavior from
this experiment was kept.

The result is useful because it separates scratch-shape reduction from full
destination coalescing. Reducing scratch from many page-sized flat buffers to
one flat buffer lowered setup timing, but it also reduced QAT byte share and
hurt elapsed performance in the focused `1M` async run.

## Artifacts

- `artifacts/contig-scratch-20260527/zfs-qat-contig-scratch-1m-jobs4-20260527.csv`
- `artifacts/contig-scratch-20260527/zfs-qat-contig-scratch-1m-jobs8-20260527.csv`
- `artifacts/contig-scratch-20260527/summary.csv`

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

| Case | Jobs | Mean elapsed ms | Mean write MiB/s | CPU active s/GiB | QAT byte share | Dst buffers/request | Setup ns/request | Wait ns/MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| page lookup cache baseline | 4 | 776.716 | 941.027 | 5.277 | 84.347% | 289.000 | 73320.183 | 32140696.333 |
| contiguous scratch | 4 | 854.918 | 855.350 | 5.240 | 79.093% | 225.000 | 45833.156 | 30929929.333 |
| delta | 4 | +10.070% | -9.105% | -0.703% | -5.253 pp | -22.145% | -37.490% | -3.767% |
| page lookup cache baseline | 8 | 989.293 | 1477.160 | 7.153 | 79.823% | 289.000 | 44507.855 | 26859470.000 |
| contiguous scratch | 8 | 1290.230 | 1137.140 | 6.395 | 72.017% | 225.000 | 37715.005 | 32966166.667 |
| delta | 8 | +30.419% | -23.019% | -10.597% | -7.807 pp | -22.145% | -15.263% | +22.736% |

## Interpretation

Do not keep this scratch-only contiguous-buffer strategy as implemented.

The structural request-shape improvement was real:

- `qat_dst_total_buffers_per_req` dropped from `289` to `225`.
- `dc_compress_add_buffers_max` dropped from `65` to `1` in the raw rows.
- Setup timing improved by about `37%` at jobs=4 and `15%` at jobs=8.

Those wins did not translate into acceptable benchmark behavior:

- jobs=4 elapsed time regressed by about `10%`.
- jobs=8 elapsed time regressed by about `30%`.
- QAT byte share fell by about `5.3` percentage points at jobs=4 and `7.8`
  percentage points at jobs=8.
- jobs=8 QAT wait cost increased despite lower local setup cost.

This reinforces the current request-overhead lesson: reducing local SGL/setup
work is not sufficient if the change shifts admission, QAT byte share, or wait
behavior in the wrong direction. Future scratch/destination work should only be
reopened with a new hypothesis that directly addresses QAT wait/service time or
fallback share, not just flat-buffer count.

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
