# QAT Scratch Reuse Full Matrix - 2026-05-23

Purpose: retest the async page-array and scratch-buffer reuse change across the
record sizes that matter for profile policy, including the required `1M` row.

## Configuration

Host: `pve.drewnet.online`

Kernel: `7.0.0-3-pve`

ZFS module:

```text
srcversion: FC2F209AA09A507B8C7F723
```

Benchmark run state:

```text
records: 64K, 128K, 256K, 512K, 1M
jobs: 1, 4, 8
iterations: 5
modes: qat, sw
verify: sw
zfs_qat_dc_profile=throughput
zfs_qat_dc_ratio_profile=balanced
zfs_qat_dc_profile_recordsize=1048576
completion mode: interrupt
QAT DC instances: 12
```

Final host state after testing:

```text
zfs_qat_dc_profile_recordsize=131072
zfs_qat_dc_profile=balanced
dc_instances=12
dc_fails=0
```

## Artifacts

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-scratch-reuse-fullmatrix-records64k128k256k512k1m-jobs1-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-scratch-reuse-fullmatrix-records64k128k256k512k1m-jobs4-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-scratch-reuse-fullmatrix-records64k128k256k512k1m-jobs8-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-scratch-reuse-fullmatrix-summary-20260523.csv
.codex/skills/openzfs-qat/artifacts/host-root-import-20260523/zfs-qat-scratch-reuse-fullmatrix-20260523-run.log
```

## Results

Positive `QAT vs SW` means QAT mode was faster than the same-window software
gzip row. QAT mode can include software fallback when the cap policy decides
not to submit to QAT.

| Record | Jobs | QAT ms | SW ms | QAT vs SW | QAT share | CPU QAT s/GiB | CPU SW s/GiB | QAT service ns/MiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 64K | 1 | 944.5 | 915.9 | 3.1% slower | 0.0% | 12.66 | 12.51 | n/a |
| 128K | 1 | 836.3 | 804.3 | 4.0% slower | 99.8% | 5.75 | 10.64 | 93,418,613 |
| 256K | 1 | 745.8 | 656.7 | 13.6% slower | 75.0% | 5.88 | 10.41 | 33,814,422 |
| 512K | 1 | 572.7 | 580.5 | 1.4% faster | 72.3% | 5.31 | 10.08 | 17,626,501 |
| 1M | 1 | 610.5 | 567.5 | 7.6% slower | 100.3% | 3.56 | 10.00 | 18,351,663 |
| 64K | 4 | 1267.0 | 1249.7 | 1.4% slower | 0.0% | 13.85 | 13.61 | n/a |
| 128K | 4 | 1157.6 | 1041.6 | 11.1% slower | 78.0% | 9.12 | 12.56 | 139,608,557 |
| 256K | 4 | 1019.5 | 981.8 | 3.8% slower | 46.8% | 8.30 | 11.93 | 40,613,710 |
| 512K | 4 | 906.6 | 957.8 | 5.3% faster | 47.6% | 7.68 | 11.39 | 20,477,195 |
| 1M | 4 | 910.6 | 920.4 | 1.1% faster | 70.3% | 6.00 | 10.92 | 33,912,126 |
| 64K | 8 | 1745.5 | 1706.3 | 2.3% slower | 0.0% | 14.15 | 13.77 | n/a |
| 128K | 8 | 1628.2 | 1496.4 | 8.8% slower | 76.1% | 8.71 | 12.82 | 137,564,084 |
| 256K | 8 | 1352.4 | 1418.4 | 4.7% faster | 57.2% | 7.68 | 12.62 | 40,726,266 |
| 512K | 8 | 1176.0 | 1434.2 | 18.0% faster | 54.2% | 7.50 | 13.71 | 20,223,740 |
| 1M | 8 | 1283.8 | 1413.9 | 9.2% faster | 76.5% | 5.98 | 12.02 | 30,917,606 |

## Interpretation

- `64K` remains software-only in QAT mode under the current max-buffer/profile
  policy, so it is not a QAT optimization target unless the project explicitly
  decides to support smaller QAT requests.
- `128K` remains slower than software in elapsed time at all tested
  concurrencies, although it cuts active CPU cost materially.
- `256K` is borderline. It loses at jobs `1` and `4`, but wins at jobs `8`.
- `512K` is the strongest current record-size target. It wins at jobs `1`, `4`,
  and `8`, with the best row at `512K/jobs=8` reaching `18.0%` faster elapsed
  than software gzip while using about `55%` of the software active CPU cost.
- `1M` is still useful, but not universally. It loses at jobs `1`, narrowly wins
  at jobs `4`, and wins clearly at jobs `8`.
- The elapsed wins at `256K`, `512K`, and `1M` are hybrid wins. They include
  both QAT work and software fallback. This is acceptable for operator-facing
  performance, but it should not be mistaken for every QAT request getting
  faster.

## Next Target

The next useful ZFS-side change is profile policy refinement, not another
mechanical allocation pass.

Recommended target:

- Keep `64K` below the QAT acceleration threshold.
- Keep `128K` conservative for throughput-sensitive profiles because elapsed
  time still regresses there.
- Favor QAT more aggressively at `512K` and `1M` when the operator profile
  target is larger-record throughput or CPU offload.
- Treat `256K` as a transition size where policy should depend on the selected
  profile bias, because the benefit only appears at higher concurrency.

Any policy change should be validated with the same matrix before becoming a
profile default.
