# Source Coalescing Request-Shape Test - 2026-05-22

Purpose: re-test QAT source coalescing with the request-overhead counters added
in step 5 before deciding whether `zfs_qat_dc_coalesce_src=profile` should
enable it automatically for any record size or profile.

## Test Shape

Host: `pve.drewnet.online`

Temporary benchmark module state:

```text
zfs_qat_dc_profile_recordsize=1048576
zfs_qat_dc_coalesce_src=0 or 1
zfs_qat_dc_coalesce_dst=0
zfs_qat_decompress_disable=1
zfs_qat_compress_disable=0
qat_pci_dh895xcc_count=2
zfs_qat_dc_instances=12
```

Matrix:

- Sync balanced profile and async throughput profile.
- `JOBS=4` and `JOBS=8`.
- Records `64K`, `128K`, `256K`, `512K`, and `1M`.
- Software rows were captured in the same windows for comparison.
- `ITERS=2`, `VERIFY_MODE=sw`, all raw rows reported `sha_ok=yes`.

Artifacts:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-src-coalesce-sync-src0-jobs4-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-src-coalesce-sync-src1-jobs4-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-src-coalesce-sync-src0-jobs8-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-src-coalesce-sync-src1-jobs8-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-src-coalesce-async-src0-jobs4-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-src-coalesce-async-src1-jobs4-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-src-coalesce-async-src0-jobs8-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-src-coalesce-async-src1-jobs8-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-src-coalesce-summary-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-step5-src-coalesce-comparison-20260522.csv
```

Post-test state was restored to the normal profile-managed boot configuration:

```text
zfs_qat_dc_profile=balanced
zfs_qat_dc_profile_recordsize=131072
zfs_qat_dc_coalesce_src=profile
zfs_qat_dc_coalesce_dst=profile
zfs_qat_decompress_disable=profile
zfs_qat_dc_poll=profile
zfs_qat_compress_disable=0
qat.service=active
dc_instances=12
dc_watchdog_health=1
```

## Sync Results

Negative elapsed delta means source coalescing was faster than the same profile
with source coalescing disabled.

| Jobs | Record | Src Off ms | Src On ms | Elapsed Delta | CPU Delta | Wait/Req Delta | Src Buffers | Copy Cost/Req | Src On vs SW |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 | 64K | 1312.5 | 1194.0 | -9.0% | -4.9% | +8.1% | 16->1 | 9.7 us | -4.7% |
| 4 | 128K | 1079.3 | 1123.8 | +4.1% | +7.8% | -1.8% | 32->1 | 17.2 us | +3.4% |
| 4 | 256K | 1048.5 | 981.6 | -6.4% | +4.6% | -1.3% | 64->1 | 40.9 us | -0.8% |
| 4 | 512K | 1057.7 | 1034.1 | -2.2% | +7.9% | -0.7% | 128->1 | 108.1 us | +6.7% |
| 4 | 1M | 1017.1 | 1002.6 | -1.4% | +13.4% | -2.0% | 256->1 | 244.0 us | +5.1% |
| 8 | 64K | 1772.3 | 1840.7 | +3.9% | +8.8% | -5.8% | 16->1 | 13.1 us | +6.8% |
| 8 | 128K | 1523.3 | 1584.1 | +4.0% | +13.6% | -4.1% | 32->1 | 34.1 us | +6.3% |
| 8 | 256K | 1448.2 | 1489.4 | +2.8% | +12.9% | -4.4% | 64->1 | 52.9 us | +9.2% |
| 8 | 512K | 1486.5 | 1466.4 | -1.4% | +16.9% | -3.1% | 128->1 | 129.9 us | +3.1% |
| 8 | 1M | 1519.6 | 1514.9 | -0.3% | +26.2% | -1.2% | 256->1 | 417.8 us | +9.5% |

Sync interpretation:

- Source coalescing always changed request shape from many source buffers to one
  source buffer per QAT request.
- That shape change did not translate into a repeatable elapsed-time win.
  `JOBS=4` had three wins, but `JOBS=8` had three clear regressions and two
  neutral rows.
- CPU cost usually increased because the copy path becomes material at larger
  records. At `1M`, copy cost was about `244 us/request` at `JOBS=4` and
  `418 us/request` at `JOBS=8`.
- Source coalescing did not make sync QAT broadly faster than software. Several
  source-on rows still lost to the same-window software row.

## Async Results

| Jobs | Record | Src Off ms | Src On ms | Elapsed Delta | CPU Delta | Wait/Req Delta | QAT Byte Share | Copy Cost/Req | Src On vs SW |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 | 64K | 1257.5 | 1317.5 | +4.8% | -1.8% | na | 0.0->0.0% | 0.0 us | +1.4% |
| 4 | 128K | 1124.7 | 1163.7 | +3.5% | +6.4% | -51.8% | 67.3->100.0% | 48.1 us | +10.7% |
| 4 | 256K | 1011.7 | 1148.2 | +13.5% | +15.7% | +17.5% | 48.1->52.1% | 280.6 us | +17.0% |
| 4 | 512K | 931.9 | 954.6 | +2.4% | +14.5% | +7.1% | 47.1->49.7% | 453.6 us | -5.3% |
| 4 | 1M | 896.3 | 905.4 | +1.0% | +10.9% | -9.6% | 77.3->88.5% | 888.4 us | -0.6% |
| 8 | 64K | 1746.5 | 1739.8 | -0.4% | +1.0% | na | 0.0->0.0% | 0.0 us | +0.3% |
| 8 | 128K | 1535.4 | 1569.6 | +2.2% | +5.4% | -14.9% | 75.6->79.1% | 37.4 us | +0.6% |
| 8 | 256K | 1366.0 | 1486.7 | +8.8% | +16.0% | -39.0% | 64.4->76.4% | 174.3 us | +6.1% |
| 8 | 512K | 1258.8 | 1392.8 | +10.6% | +8.5% | -7.3% | 62.0->69.7% | 322.8 us | -5.4% |
| 8 | 1M | 1374.8 | 1367.1 | -0.6% | +25.4% | +15.0% | 82.4->80.2% | 1071.0 us | -7.3% |

Async interpretation:

- Async source coalescing was mostly worse on elapsed time: seven regressions,
  three neutral rows, and no clear win by the `2%` threshold.
- Some rows showed lower wait per completed QAT request, but elapsed time and
  CPU cost still regressed. This is exactly the metric trap described in
  `benchmark-evaluation-methodology.md`: request-level timing is not enough if
  admission share, copy cost, and same-window software results move too.
- Source coalescing often increased QAT byte share by making more requests
  admissible, but those extra QAT requests were not faster than the hybrid
  fallback policy.
- `64K` async rows remained software-fallback rows under the current cap policy,
  so source coalescing did not apply there.

## Decision

Do not enable source coalescing from `profile` at this stage.

Current profile behavior should remain:

```text
zfs_qat_dc_coalesce_src=profile -> effective off
zfs_qat_dc_coalesce_dst=profile -> effective off
```

Rationale:

- Reducing source buffers per request is real, but copy cost scales directly
  with record size and is large enough to erase or reverse the benefit.
- The wins are not repeatable across concurrency. A profile default must work
  across typical single-card and dual-card operation, not only one row.
- Async source coalescing risks increasing QAT utilization in rows where hybrid
  fallback is currently the better user-facing policy.
- There is no compression-ratio reason to prefer source coalescing; the decision
  is latency, throughput, CPU, and offload-share driven.

Source coalescing remains useful as a manual diagnostic knob:

```text
zfs_qat_dc_coalesce_src=1
```

Use it when testing whether a particular platform's QAT service time is
scatter/gather sensitive. Do not document it as a general performance default.

## Next Target

The next ZFS-side optimization should be narrower than broad source coalescing:

- Instrument source and destination pointer alignment, segment lengths, and SGL
  alignment so we can determine whether the QAT path is violating documented
  alignment guidance often enough to matter.
- If misalignment is common, test a targeted alignment fix that only copies the
  bad shape instead of coalescing every source buffer.
- Continue QAT-side platform work in parallel: service split, PCIe link state,
  polling/interrupt behavior, parameter checking, and NUMA hygiene.

Do not promote additional profile defaults until a candidate improves elapsed
time or CPU cost without relying on lower QAT byte share or an unacceptable copy
cost.
