# QAT Parameter-Checking Experiment - 2026-05-23

Purpose: test whether disabling QAT 4.28 access-layer parameter checking reduces
OpenZFS QAT gzip latency on QAT 1.x `dh895xcc` hardware.

## Build Control

The QAT DKMS packaging now supports:

```text
QAT_DKMS_PARAM_CHECK=y
QAT_DKMS_PARAM_CHECK=n
```

The first attempt failed because QAT `configure` expects `yes` or `no`, not `y`
or `n`. The QAT submodule was fixed to map:

```text
y -> --enable-param-check=yes
n -> --enable-param-check=no
```

The successful experiment used a guarded rebuild flow:

```text
1. Build and install QAT DKMS with QAT_DKMS_PARAM_CHECK=n.
2. Rebuild and install ZFS DKMS against /usr/src/qat-4.28.0-00004.
3. Regenerate initramfs only after both DKMS builds succeeded.
4. Reboot and verify QAT/ZFS health.
5. Run smoke test and benchmark.
6. Restore QAT_DKMS_PARAM_CHECK=y, rebuild ZFS, restore default ZFS boot options,
   regenerate initramfs, and reboot.
```

Artifacts:

```text
.codex/skills/openzfs-qat/artifacts/qat-param-check-off-build-20260523.log
.codex/skills/openzfs-qat/artifacts/qat-param-check-off-zfs-rebuild-20260523.log
.codex/skills/openzfs-qat/artifacts/qat-param-check-restore-on-20260523.log
.codex/skills/openzfs-qat/artifacts/zfs-qat-paramcheck-off-throughput-1m-jobs4-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-paramcheck-off-throughput-1m-jobs8-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-paramcheck-summary-20260523.csv
```

## Smoke Test

After booting the parameter-check-off build:

```text
kernel: 7.0.0-3-pve
qat.service: active
qat_dev0: dh895xcc, state up
qat_dev1: dh895xcc, state up
dc_instances=12
dc_watchdog_health=1
```

A temporary `test-hdd-pool` dataset was created with `compression=gzip-1` and
`recordsize=128K`. The payload checksum verified successfully.

Smoke counters:

```text
comp_requests: 0 -> 2496
comp_total_in_bytes: 0 -> 327155712
comp_total_out_bytes: 0 -> 3477888
dc_fails: 0 -> 0
dc_instances: 12
dc_watchdog_health: 1
```

## Benchmark

Benchmark target:

```text
profile=throughput
recordsize=1M
zfs_qat_dc_profile_recordsize=1048576
zfs_qat_decompress_disable=1
MODES="qat sw"
ITERS=2
JOBS=4 and JOBS=8
VERIFY_MODE=sw
```

Comparable parameter-check-on baseline:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-service-split-summary-20260522.csv
variant=cydc_kernelcy0
```

Summary from raw rows:

| Jobs | Param Check | Mode | Avg elapsed ms | Avg CPU active s/GiB | Avg QAT byte share | Avg QAT service ns/MiB |
|---:|---|---|---:|---:|---:|---:|
| 4 | on | QAT | 906.954 | 5.090 | 90.29% | 28,467,752 |
| 4 | off | QAT | 950.530 | 5.193 | 84.26% | 30,048,801 |
| 4 | off | SW | 905.956 | 11.402 | 0.00% | n/a |
| 8 | on | QAT | 1296.981 | 6.477 | 76.82% | 32,173,393 |
| 8 | off | QAT | 1323.918 | 6.236 | 74.91% | 30,736,659 |
| 8 | off | SW | 1481.804 | 12.063 | 0.00% | n/a |

Direct QAT-to-QAT comparison:

| Jobs | Elapsed change with param checking disabled |
|---:|---:|
| 4 | 4.80% slower |
| 8 | 2.08% slower |

## Decision

Do not disable QAT access-layer parameter checking as a performance
optimization.

This experiment did not produce an elapsed-latency win:

- `JOBS=4` regressed versus both the parameter-check-on QAT baseline and the
  same-run software gzip result.
- `JOBS=8` remained faster than software gzip, but was still slower than the
  parameter-check-on QAT baseline.
- CPU cost remained materially lower than software gzip, but CPU reduction alone
  is not enough to justify a build option that worsens elapsed latency.

Parameter checking was restored to the default enabled state after the
benchmark.

## Final Host State

After restoration and reboot:

```text
kernel: 7.0.0-3-pve
qat.service: active
qat_dev0: dh895xcc, state up
qat_dev1: dh895xcc, state up
QAT dkms.conf: MAKE[0]="./scripts/dkms-build.sh ${kernel_source_dir} ${kernelver}"
zfs_qat_dc_profile=balanced
zfs_qat_dc_profile_recordsize=131072
zfs_qat_decompress_disable=profile
zfs_qat_dc_max_buf_size=profile
zfs_qat_compress_disable=0
dc_instances=12
dc_watchdog_health=1
dc_fails=0
```

## Next Target

The next QAT-side target should not be parameter checking. Prefer a controlled
QAT instance completion-mode experiment, because the QAT runtime config still
uses interrupt mode for `[KERNEL_QAT]` DC instances:

```text
Dc0..Dc5IsPolled = 0
```

Any polling-mode experiment must keep the same DKMS/initramfs guardrails used
here: build first, verify module compatibility, and only update initramfs after
successful QAT and ZFS rebuilds.
