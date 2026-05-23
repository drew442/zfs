# QAT Profile Minimum Buffer Policy - 2026-05-23

Purpose: make throughput and latency profiles avoid small QAT DC requests that
have repeatedly regressed elapsed time, while preserving the existing balanced
default and the explicit CPU-offload profile behavior.

## Change

Added a profile-capable module parameter:

```text
zfs_qat_dc_min_buf_size=profile
```

Accepted manual values are:

```text
8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576
```

Profile mapping:

| QAT DC profile | Effective minimum QAT DC size |
|---|---:|
| balanced | 8192 |
| latency | 524288 |
| throughput | 524288 |
| offload | 8192 |

The threshold applies to both compression and decompression acceleration
eligibility. It is a runtime policy threshold and does not require QAT DC
session reinitialization. If an operator sets a manual value above the effective
maximum buffer size, QAT DC compression/decompression is skipped by policy.

The lock-step helper now writes `zfs_qat_dc_min_buf_size=profile` in both
`apply` and `restore-default` mode so stale manual thresholds do not survive a
profile reset.

## Validation

Build and boot validation on `pve.drewnet.online`:

```text
kernel: 7.0.0-3-pve
zfs srcversion: 636FD59ED8E9ADFD8C60AC1
zfs_qat_dc_min_buf_size=profile
zfs_qat_dc_max_buf_size=profile
dc_instances=12
dc_fails=0
qat.service=active
```

Focused benchmark configuration:

```text
profile: throughput
ratio profile: balanced
profile recordsize: 1048576
effective min buffer size: 524288
effective max buffer size: 1048576
records: 128K, 256K, 512K, 1M
jobs: 1, 4, 8
iterations: 3
modes: qat, sw
verify: sw
```

Artifacts:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-policy-summary-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-policy-records128k256k512k1m-jobs1-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-policy-records128k256k512k1m-jobs4-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-policy-records128k256k512k1m-jobs8-20260523.csv
.codex/skills/openzfs-qat/artifacts/host-root-import-20260523/zfs-qat-minbuf-policy-20260523-run.log
.codex/skills/openzfs-qat/artifacts/host-root-import-20260523/zfs-qat-20260523-minbuf-dkms-build.log
.codex/skills/openzfs-qat/artifacts/host-root-import-20260523/zfs-qat-20260523-minbuf-dkms-install.log
.codex/skills/openzfs-qat/artifacts/host-root-import-20260523/zfs-qat-20260523-minbuf-initramfs.log
```

## Results

Negative `QAT vs SW` means QAT mode completed faster than same-window software
gzip. For 128K and 256K rows, QAT mode intentionally used software because the
profile threshold skipped QAT DC.

| Record | Jobs | QAT ms | SW ms | QAT vs SW | QAT share | CPU QAT s/GiB | CPU SW s/GiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| 128K | 1 | 824.6 | 811.4 | 1.6% slower | 0.0% | 11.50 | 10.66 |
| 256K | 1 | 684.5 | 711.8 | 3.8% faster | 0.0% | 10.66 | 10.21 |
| 512K | 1 | 578.9 | 589.5 | 1.8% faster | 75.7% | 5.26 | 10.17 |
| 1M | 1 | 626.2 | 568.7 | 10.1% slower | 100.3% | 3.70 | 9.36 |
| 128K | 4 | 1086.1 | 1103.4 | 1.6% faster | 0.0% | 15.78 | 17.13 |
| 256K | 4 | 1098.7 | 1161.2 | 5.4% faster | 0.0% | 19.03 | 19.39 |
| 512K | 4 | 1002.0 | 1127.8 | 11.2% faster | 48.2% | 11.75 | 19.20 |
| 1M | 4 | 899.2 | 1050.1 | 14.4% faster | 82.2% | 5.93 | 16.59 |
| 128K | 8 | 1833.7 | 1533.2 | 19.6% slower | 0.0% | 23.47 | 12.99 |
| 256K | 8 | 1801.8 | 1459.5 | 23.5% slower | 0.0% | 22.94 | 12.18 |
| 512K | 8 | 1611.0 | 1537.0 | 4.8% slower | 45.4% | 14.15 | 16.63 |
| 1M | 8 | 1303.9 | 1648.0 | 20.9% faster | 77.1% | 6.07 | 15.14 |

## Interpretation

- The policy works as intended: under `throughput`, 128K and 256K rows reported
  effective min size `524288`, zero QAT compression requests, and zero QAT byte
  share.
- `512K` and `1M` still offload to QAT under the same profile and preserve the
  major CPU reduction seen in earlier runs.
- The best elapsed rows in this pass were `1M/jobs=8`, `1M/jobs=4`, and
  `512K/jobs=4`.
- The `jobs=8` 128K/256K rows were slower even though QAT was skipped. Because
  those rows submitted no QAT work, this should be treated as benchmark-window
  or storage-pool variability rather than a QAT engine regression.
- The policy is useful as a throughput/latency guardrail, but it does not solve
  the remaining mixed behavior at high concurrency. Future benchmark decisions
  should continue using same-window software rows and QAT-share metrics.

## Host State After Testing

The host was restored to the safe default profile and rebooted:

```text
zfs_qat_dc_profile=balanced
zfs_qat_dc_profile_recordsize=131072
zfs_qat_dc_min_buf_size=profile
zfs_qat_dc_max_buf_size=profile
dc_instances=12
dc_fails=0
```

## Next Target

The next useful target is a more stable elapsed benchmark path, not another QAT
request-shape change. The current HDD-backed benchmark pool is noisy enough
that software-only rows can diverge between `qat` and `sw` modes even when QAT
submits are zero. Before promoting more profile defaults, run the same policy
on a lower-variance target, preferably a scratch NVMe-backed dataset with the
same source file and the required `1M` row.
