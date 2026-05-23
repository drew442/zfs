# QAT Kernel Instance Polling - 2026-05-23

Purpose: test QAT 4.28 `[KERNEL_QAT]` DC completion mode on `dh895xcc` hardware
by changing kernel QAT DC instances from interrupt mode to polling mode.

## Configuration

Baseline QAT config:

```text
[GENERAL]
ServicesEnabled = cy;dc

[KERNEL]
NumberCyInstances = 0
NumberDcInstances = 0

[KERNEL_QAT]
NumberCyInstances = 0
NumberDcInstances = 6
Dc0..Dc5IsPolled = 0
```

Experiment QAT config:

```text
[KERNEL_QAT]
NumberCyInstances = 0
NumberDcInstances = 6
Dc0..Dc5IsPolled = 1
```

Both `/etc/dh895xcc_dev0.conf` and `/etc/dh895xcc_dev1.conf` were backed up
before editing:

```text
/etc/dh895xcc_dev0.conf.pre-20260523-kernelqat-dc-poll
/etc/dh895xcc_dev1.conf.pre-20260523-kernelqat-dc-poll
```

Important compatibility finding:

```text
driver=poll and zfs=interrupt is rejected by OpenZFS QAT init
```

The first reboot with `DcNIsPolled=1` but default `zfs_qat_dc_poll=profile`
produced:

```text
WARNING: QAT DC instance 0 poll mode mismatch: driver=poll zfs=interrupt.
Set DcNIsPolled and zfs_qat_dc_poll to matching values before QAT DC init
```

ZFS then kept QAT compression disabled:

```text
dc_instances=0
zfs_qat_compress_disable=1
```

The successful polling run used matching boot options:

```text
zfs_qat_dc_poll=1
zfs_qat_dc_profile_recordsize=1048576
zfs_qat_decompress_disable=1
```

## Smoke Test

After rebooting with both QAT driver DC instances and ZFS in poll mode:

```text
qat.service=active
qat_dev0=up
qat_dev1=up
dc_instances=12
dc_watchdog_health=1
```

A temporary `test-hdd-pool` dataset was created with `compression=gzip-1` and
`recordsize=1M`. The payload checksum verified successfully.

Smoke counters:

```text
comp_requests: 0 -> 352
comp_total_in_bytes: 0 -> 369098752
comp_total_out_bytes: 0 -> 5270016
dc_poll_calls: 0 -> 519864
dc_poll_success: 0 -> 297
dc_poll_fails: 0 -> 0
dc_fails: 0 -> 0
dc_instances: 12
dc_watchdog_health: 1
```

## Benchmark

Benchmark target:

```text
profile=throughput
recordsize=1M
MODES="qat sw"
ITERS=2
JOBS=4 and JOBS=8
VERIFY_MODE=sw
```

Artifacts:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-kernelqat-poll-throughput-1m-jobs4-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-kernelqat-poll-throughput-1m-jobs8-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-kernelqat-poll-summary-20260523.csv
```

Comparable interrupt baseline:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-service-split-summary-20260522.csv
variant=cydc_kernelcy0
```

Summary from raw rows:

| Jobs | Completion Mode | Mode | Avg elapsed ms | Avg CPU active s/GiB | Avg QAT byte share | Avg QAT service ns/MiB | Avg poll calls | Avg poll successes |
|---:|---|---|---:|---:|---:|---:|---:|---:|
| 4 | interrupt | QAT | 906.954 | 5.090 | 90.29% | 28,467,752 | n/a | n/a |
| 4 | poll | QAT | 880.826 | 5.651 | 76.10% | 33,226,070 | 698,778 | 500 |
| 4 | poll | SW | 941.119 | 11.294 | 0.00% | n/a | 0 | 0 |
| 8 | interrupt | QAT | 1296.981 | 6.477 | 76.82% | 32,173,393 | n/a | n/a |
| 8 | poll | QAT | 1231.251 | 6.161 | 76.11% | 31,952,427 | 1,369,524 | 1,014 |
| 8 | poll | SW | 1462.120 | 12.206 | 0.00% | n/a | 0 | 0 |

Direct QAT-to-QAT elapsed comparison:

| Jobs | Poll elapsed change vs interrupt |
|---:|---:|
| 4 | 2.88% faster |
| 8 | 5.07% faster |

## Interpretation

Polling mode is the first QAT-side experiment in this sequence to show a useful
elapsed-time improvement against the comparable interrupt baseline.

The strength of the result differs by concurrency:

- `JOBS=8` is the cleaner result. QAT byte share is nearly unchanged versus the
  interrupt baseline, CPU cost is slightly lower, QAT service ns/MiB is slightly
  lower, and elapsed time improves by about 5%.
- `JOBS=4` improves elapsed time, but QAT byte share drops from about 90% to
  about 76%. Treat that row as a hybrid fallback win rather than a pure QAT
  engine win.

Polling mode also makes the ZFS/QAT configuration more coupled. ZFS and the QAT
driver must agree on completion mode before QAT DC initialization. A mismatch
is safely rejected, but it leaves QAT compression disabled until configuration
is corrected and the module is reloaded or the host is rebooted.

## Decision

Keep QAT `[KERNEL_QAT]` polling mode in scope for a larger completion-mode
matrix. Do not promote it as the default yet from one two-iteration run.

Recommended next validation:

```text
1. Repeat poll vs interrupt with more iterations.
2. Include 128K and 1M records.
3. Include JOBS=4, JOBS=8, and a higher-concurrency point if stable.
4. Track QAT byte share beside elapsed time so fallback-driven wins are visible.
5. Evaluate whether a ZFS/QAT profile should set zfs_qat_dc_poll=1 only when
   QAT runtime config also has DcNIsPolled=1.
```

Follow-up completed: the expanded completion-mode matrix, polling interval and
quota sweeps, and lock-step helper are documented in
`qat-completion-mode-lockstep-20260523.md`. That follow-up did not justify
promoting polling to a profile default.

## Final Host State

After the benchmark, the host was restored to default interrupt-mode QAT config
and default/profile ZFS boot options:

```text
[KERNEL_QAT] Dc0..Dc5IsPolled = 0
zfs_qat_dc_profile=balanced
zfs_qat_dc_profile_recordsize=131072
zfs_qat_decompress_disable=profile
zfs_qat_dc_poll=profile
zfs_qat_compress_disable=0
dc_instances=12
dc_watchdog_health=1
dc_fails=0
```
