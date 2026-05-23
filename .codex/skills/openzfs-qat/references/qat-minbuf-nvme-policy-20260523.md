# QAT Min-Buffer Policy NVMe Validation - 2026-05-23

Purpose: retest the profile-driven minimum QAT DC request-size policy on a
lower-variance NVMe-backed benchmark dataset instead of the HDD benchmark pool.

## Configuration

Host: `pve.drewnet.online`

Kernel and module:

```text
kernel: 7.0.0-3-pve
zfs srcversion: 636FD59ED8E9ADFD8C60AC1
QAT DC instances: 12
dc_fails: 0
```

Benchmark root:

```text
nvme_scratch/bench
```

Runtime policy:

```text
zfs_qat_dc_profile=throughput
zfs_qat_dc_ratio_profile=balanced
zfs_qat_dc_profile_recordsize=1048576
zfs_qat_dc_min_buf_size=profile
zfs_qat_dc_effective_min_buf_size=524288
zfs_qat_dc_max_buf_size=profile
zfs_qat_dc_effective_max_buf_size=1048576
```

Benchmark matrix:

```text
records: 128K, 256K, 512K, 1M
jobs: 1, 4, 8
iterations: 3
modes: qat, sw
verify: sw
run order: mode
```

The host was restored to the default balanced `128K` profile after testing and
temporary `nvme_scratch/bench/qat-phase4-*` datasets were destroyed.

## Artifacts

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-nvme-policy-summary-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-nvme-policy-records128k256k512k1m-jobs1-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-nvme-policy-records128k256k512k1m-jobs4-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-nvme-policy-records128k256k512k1m-jobs8-20260523.csv
.codex/skills/openzfs-qat/artifacts/host-root-import-20260523/zfs-qat-minbuf-nvme-policy-20260523-run.log
```

## Results

Negative `QAT vs SW` means QAT mode completed faster than same-window software
gzip. For 128K and 256K rows, QAT mode intentionally used software because the
throughput profile threshold skipped QAT DC.

| Record | Jobs | QAT ms | SW ms | QAT vs SW | QAT share | CPU QAT s/GiB | CPU SW s/GiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| 128K | 1 | 574.7 | 569.7 | 0.9% slower | 0.0% | 11.09 | 9.97 |
| 256K | 1 | 573.3 | 556.4 | 3.0% slower | 0.0% | 11.68 | 9.99 |
| 512K | 1 | 529.9 | 521.6 | 1.6% slower | 82.2% | 5.28 | 9.81 |
| 1M | 1 | 548.5 | 538.6 | 1.8% slower | 100.3% | 3.70 | 9.91 |
| 128K | 4 | 904.1 | 864.5 | 4.6% slower | 0.0% | 16.28 | 15.70 |
| 256K | 4 | 962.7 | 960.6 | 0.2% slower | 0.0% | 18.31 | 18.68 |
| 512K | 4 | 858.8 | 919.4 | 6.6% faster | 42.1% | 12.35 | 15.74 |
| 1M | 4 | 858.0 | 803.7 | 6.8% slower | 88.7% | 5.39 | 14.82 |
| 128K | 8 | 1338.1 | 1328.8 | 0.7% slower | 0.0% | 23.61 | 20.85 |
| 256K | 8 | 1137.4 | 1418.9 | 19.8% faster | 0.0% | 18.90 | 21.35 |
| 512K | 8 | 931.6 | 1277.8 | 27.1% faster | 55.6% | 9.35 | 19.09 |
| 1M | 8 | 1247.4 | 1439.8 | 13.4% faster | 80.9% | 8.23 | 20.80 |

## Interpretation

- The policy still behaves correctly on NVMe: 128K and 256K rows report zero
  QAT requests and zero QAT byte share under `throughput`.
- The high-concurrency 128K regression from the HDD-backed pass did not repeat
  on NVMe. The 128K/jobs=8 row is nearly neutral instead of 19.6% slower.
- `512K` remains the best elapsed-throughput target. It wins at jobs `4` and
  `8`, and at jobs `8` it is 27.1% faster than software while using about half
  the active CPU cost.
- `1M` remains mixed. It wins strongly at jobs `8`, loses at jobs `1`, and lost
  in this NVMe jobs `4` pass despite materially lower CPU cost.
- `256K/jobs=8` is faster even with zero QAT submits. This proves the benchmark
  can still produce mode-window differences unrelated to QAT engine work.

## Harness Update

The benchmark harness now accepts:

```text
RUN_ORDER=mode
RUN_ORDER=record
```

`mode` preserves the existing behavior: run all records for one mode, then the
next mode. `record` runs all modes for each record before moving to the next
record, which should reduce drift when comparing QAT and software rows for the
same record size.

Runtime validation: a default-profile smoke run with `RUN_ORDER=record`,
`RECORDS=1M`, `JOBS=1`, `ITERS=1`, and `MODES="qat sw"` completed
successfully on `nvme_scratch/bench`.

## Next Target

Before changing additional profile defaults, rerun the NVMe matrix with
`RUN_ORDER=record`. That will give a better same-record comparison for rows
where QAT is skipped by policy and should make it easier to distinguish actual
QAT performance from benchmark-window drift.
