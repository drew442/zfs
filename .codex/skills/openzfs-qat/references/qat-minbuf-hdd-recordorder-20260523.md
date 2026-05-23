# QAT Min-Buffer HDD Paired Run-Order Validation - 2026-05-23

Purpose: rerun the paired min-buffer policy matrix on `test-hdd-pool` to check
whether slower rotational media changes the preferred QAT profile decision
relative to the NVMe-backed pass.

The run was executed on 2026-05-23 UTC. The raw artifact filenames use the
`20260524` label because that label was passed to the benchmark output paths;
the host UTC check after restore was `2026-05-23T23:47:39Z`.

## Configuration

Host: `pve.drewnet.online`

```text
kernel: 7.0.0-3-pve
zfs srcversion: 636FD59ED8E9ADFD8C60AC1
QAT DC instances: 12
dc_fails: 0
benchmark root: test-hdd-pool/bench
```

Runtime policy during the run:

```text
zfs_qat_dc_profile=throughput
zfs_qat_dc_ratio_profile=balanced
zfs_qat_dc_profile_recordsize=1048576
zfs_qat_dc_min_buf_size=profile
zfs_qat_dc_effective_min_buf_size=524288
zfs_qat_dc_max_buf_size=profile
zfs_qat_dc_effective_max_buf_size=1048576
RUN_ORDER=record
```

Matrix:

```text
records: 128K, 256K, 512K, 1M
jobs: 1, 4, 8
iterations: 3
modes: qat, sw
verify: sw
```

Temporary `test-hdd-pool/bench/qat-phase4-*` datasets were destroyed after the
run. Existing non-temporary children under `test-hdd-pool/bench` were left
untouched.

The host was restored to default `balanced` profile and `128K` profile target
after testing.

## Artifacts

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-hdd-recordorder-summary-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-hdd-recordorder-records128k256k512k1m-jobs1-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-hdd-recordorder-records128k256k512k1m-jobs4-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-hdd-recordorder-records128k256k512k1m-jobs8-20260524.csv
.codex/skills/openzfs-qat/artifacts/host-root-import-20260523/zfs-qat-minbuf-hdd-recordorder-20260524-run.log
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-media-comparison-20260523.csv
```

All three raw HDD CSVs have 246 columns on every row.

## HDD Results

Negative `QAT vs SW` means the QAT-labelled policy row completed faster than
same-record software gzip. For `128K` and `256K`, QAT share is zero by design
because the throughput profile skips QAT below the `512K` minimum.

| Record | Jobs | QAT ms | SW ms | QAT vs SW | QAT share | CPU QAT s/GiB | CPU SW s/GiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| 128K | 1 | 869.0 | 831.3 | 4.5% slower | 0.0% | 11.13 | 10.95 |
| 128K | 4 | 1056.6 | 1087.9 | 2.9% faster | 0.0% | 12.50 | 12.48 |
| 128K | 8 | 1486.3 | 1500.3 | 0.9% faster | 0.0% | 12.67 | 12.55 |
| 256K | 1 | 720.1 | 714.9 | 0.7% slower | 0.0% | 10.30 | 10.37 |
| 256K | 4 | 1007.6 | 964.5 | 4.5% slower | 0.0% | 11.81 | 11.76 |
| 256K | 8 | 1375.4 | 1380.5 | 0.4% faster | 0.0% | 12.54 | 12.30 |
| 512K | 1 | 593.3 | 600.6 | 1.2% faster | 76.8% | 5.09 | 9.77 |
| 512K | 4 | 944.2 | 995.7 | 5.2% faster | 47.1% | 7.74 | 11.36 |
| 512K | 8 | 1161.5 | 1506.9 | 22.9% faster | 52.1% | 7.66 | 12.15 |
| 1M | 1 | 628.5 | 571.6 | 10.0% slower | 100.3% | 3.88 | 9.55 |
| 1M | 4 | 874.4 | 914.0 | 4.3% faster | 74.4% | 5.55 | 11.39 |
| 1M | 8 | 1270.6 | 1399.9 | 9.2% faster | 80.1% | 5.99 | 11.85 |

## Media Comparison

The paired NVMe result had `1M` slower than software at jobs `1`, `4`, and `8`.
The paired HDD result has `1M` slower only at jobs `1`; it wins elapsed time at
jobs `4` and `8` while also roughly halving active CPU seconds per GiB.

| Record | Jobs | HDD QAT vs SW | NVMe QAT vs SW |
|---|---:|---:|---:|
| 512K | 1 | 1.2% faster | 4.5% faster |
| 512K | 4 | 5.2% faster | 7.7% faster |
| 512K | 8 | 22.9% faster | 5.9% faster |
| 1M | 1 | 10.0% slower | 2.7% slower |
| 1M | 4 | 4.3% faster | 6.7% slower |
| 1M | 8 | 9.2% faster | 4.7% slower |

## Interpretation

- `512K` remains the safest elapsed-time profile target across both HDD and
  NVMe. It wins at all tested concurrency levels on both media classes.
- HDD does show a media-sensitive difference at `1M`: it becomes an elapsed-time
  win at jobs `4` and `8`, while NVMe did not.
- The `1M` HDD result supports the theory that slower or more I/O-bound media
  can benefit more from higher compression ratio and reduced write volume, but
  the result is not broad enough to justify a default media-bias API yet.
- `128K` and `256K` remain software-only policy-window rows under this
  throughput profile and should not be used as QAT engine evidence.
- CPU savings remain substantial for `512K` and `1M` on HDD, even when elapsed
  time is neutral or negative.

## Next Target

Keep `512K` as the current candidate for the throughput profile's minimum QAT
request size and large-record target. Before adding explicit `rotational` or
`flash` profile inputs, test whether the same HDD/NVMe split holds across other
source data and at least one less-compressible workload. If the split repeats,
make storage-media bias a profile input rather than a manual low-level tuning
requirement.
