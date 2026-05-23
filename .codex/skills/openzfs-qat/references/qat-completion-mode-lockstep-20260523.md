# QAT Completion Mode Lock-Step - 2026-05-23

Purpose: complete the expanded QAT DC interrupt-vs-polling evaluation and add
an operator helper for the coupled QAT driver/ZFS polling configuration.

## Operational Rule

QAT DC completion mode is a lock-step boot-time setting:

```text
QAT driver [KERNEL_QAT] DcNIsPolled == ZFS zfs_qat_dc_poll effective value
```

OpenZFS now refuses QAT DC initialization on mismatch. That is safer than
stranding requests, but the result is still operationally disruptive:
`dc_instances=0` and QAT compression remains unavailable until configuration is
fixed and ZFS/QAT is reinitialized, normally by reboot.

The default state remains interrupt mode:

```text
[KERNEL_QAT] Dc0..Dc5IsPolled = 0
zfs_qat_dc_poll=profile
zfs_qat_dc_profile=balanced
```

## Helper

`contrib/qat/zfs-qat-lockstep-config.sh` manages the coupled settings:

```bash
contrib/qat/zfs-qat-lockstep-config.sh show
contrib/qat/zfs-qat-lockstep-config.sh apply --completion poll --recordsize 1048576 --decompress-disable 1 --update-initramfs
contrib/qat/zfs-qat-lockstep-config.sh apply --completion interrupt --recordsize 1048576 --decompress-disable 1 --update-initramfs
contrib/qat/zfs-qat-lockstep-config.sh restore-default --update-initramfs
```

The helper:

- updates `/etc/dh895xcc_dev*.conf` and `/etc/c6xx_dev*.conf` when present;
- rewrites only `[KERNEL_QAT] DcNIsPolled` values;
- writes matching `/etc/modprobe.d/zfs-qat.conf` `zfs_qat_dc_poll` values;
- creates timestamped backups before editing;
- optionally runs `update-initramfs -u -k "$(uname -r)"`;
- does not use or install dracut.

Reboot after applying or restoring completion mode. Live writes to
`zfs_qat_dc_poll` are intentionally not the operational path because QAT DC
instances are initialized once.

## Expanded Matrix

Configuration:

```text
records: 128K, 1M
jobs: 4, 8, 12
modes: qat, sw
iterations: 5
verify: sw
profile: throughput
ratio profile: balanced
QAT devices: 2 x DH895XCC
ZFS QAT DC instances: 12
```

Artifacts:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-completion-interrupt-records128k1m-jobs4-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-completion-interrupt-records128k1m-jobs8-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-completion-interrupt-records128k1m-jobs12-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-completion-poll-records128k1m-jobs4-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-completion-poll-records128k1m-jobs8-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-completion-poll-records128k1m-jobs12-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-completion-mode-matrix-summary-20260523.csv
```

QAT rows:

| Completion | Record | Jobs | Avg elapsed ms | QAT vs SW elapsed | Poll vs interrupt | QAT byte share | CPU active s/GiB | QAT service ns/MiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| interrupt | 128K | 4 | 1142.2 | 5.7% slower | n/a | 73.8% | 9.12 | 140,109,241 |
| poll | 128K | 4 | 1176.7 | 2.3% slower | 3.0% slower | 77.4% | 9.40 | 141,385,974 |
| interrupt | 128K | 8 | 1669.8 | 13.2% slower | n/a | 72.7% | 8.28 | 142,767,660 |
| poll | 128K | 8 | 1858.8 | 17.4% slower | 11.3% slower | 78.9% | 11.48 | 85,016,823 |
| interrupt | 128K | 12 | 1838.1 | 0.3% slower | n/a | 67.0% | 9.61 | 143,349,755 |
| poll | 128K | 12 | 1846.6 | 5.5% slower | 0.5% slower | 67.5% | 9.84 | 147,088,992 |
| interrupt | 1M | 4 | 868.2 | 6.2% faster | n/a | 76.6% | 5.60 | 32,492,735 |
| poll | 1M | 4 | 933.5 | 14.1% faster | 7.5% slower | 88.2% | 5.23 | 31,469,904 |
| interrupt | 1M | 8 | 1290.9 | 13.2% faster | n/a | 78.5% | 6.01 | 28,734,206 |
| poll | 1M | 8 | 1573.9 | 11.4% slower | 21.9% slower | 82.1% | 7.82 | 19,100,634 |
| interrupt | 1M | 12 | 1591.7 | 2.0% faster | n/a | 71.3% | 7.23 | 26,752,333 |
| poll | 1M | 12 | 1456.8 | 21.2% faster | 8.5% faster | 65.7% | 7.01 | 32,280,357 |

## Poll Tuning

The focused tuning target was `1M` because the expanded matrix showed polling
was only competitive for larger records and higher concurrency.

Interval sweep:

```text
record: 1M
jobs: 8, 12
mode: qat
iterations: 3
quota: profile/unbounded
```

Artifact:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval-sweep-summary-20260523.csv
```

| Interval us | Jobs | Avg elapsed ms | CPU active s/GiB | QAT byte share | QAT service ns/MiB | Poll calls |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | 8 | 1364.0 | 6.96 | 81.7% | 25,501,861 | 1,530,584 |
| 0 | 12 | 1584.6 | 7.70 | 71.8% | 28,170,280 | 1,957,836 |
| 1 | 8 | 1299.4 | 6.02 | 77.1% | 31,328,111 | 168,968 |
| 1 | 12 | 1764.9 | 7.90 | 73.3% | 25,310,454 | 252,204 |
| 5 | 8 | 1267.2 | 6.27 | 73.6% | 32,522,420 | 129,628 |
| 5 | 12 | 1486.1 | 7.14 | 68.0% | 30,108,461 | 180,812 |
| 10 | 8 | 1368.0 | 6.53 | 81.6% | 24,554,868 | 120,440 |
| 10 | 12 | 2042.8 | 10.36 | 70.5% | 34,248,777 | 204,316 |
| 25 | 8 | 1425.8 | 6.84 | 80.9% | 22,424,381 | 81,200 |
| 25 | 12 | 1796.6 | 8.46 | 74.2% | 27,002,566 | 104,532 |
| 50 | 8 | 1448.3 | 6.76 | 82.1% | 19,023,830 | 49,400 |
| 50 | 12 | 1679.8 | 8.17 | 74.5% | 25,115,719 | 68,052 |

Quota sweep:

```text
record: 1M
jobs: 8, 12
mode: qat
iterations: 3
interval: 5us
```

Artifact:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-quota-sweep-summary-20260523.csv
```

| Quota | Jobs | Avg elapsed ms | CPU active s/GiB | QAT byte share | QAT service ns/MiB | Poll calls |
|---:|---:|---:|---:|---:|---:|---:|
| profile | 8 | 1398.5 | 6.60 | 81.6% | 21,485,577 | 147,464 |
| 1 | 8 | 1232.9 | 5.94 | 77.8% | 31,309,200 | 140,224 |
| 4 | 8 | 1252.1 | 5.93 | 77.5% | 30,341,221 | 137,856 |
| 16 | 8 | 1271.1 | 6.24 | 81.6% | 27,323,738 | 146,900 |
| 64 | 8 | 1406.3 | 7.05 | 77.7% | 25,609,376 | 146,136 |
| profile | 12 | 1542.6 | 7.16 | 70.6% | 25,443,609 | 187,712 |
| 1 | 12 | 1470.8 | 7.10 | 65.7% | 32,186,012 | 172,224 |
| 4 | 12 | 1495.9 | 7.00 | 68.7% | 31,676,597 | 178,392 |
| 16 | 12 | 1773.4 | 9.05 | 73.1% | 30,131,382 | 199,548 |
| 64 | 12 | 1690.3 | 7.90 | 73.1% | 26,886,003 | 199,020 |

## Decision

Do not make polling profile-defaulted yet.

Reasons:

- Polling is worse than interrupt for all tested `128K` QAT rows.
- Polling is worse for `1M/jobs=4` and `1M/jobs=8` in the full matrix.
- Polling only wins clearly at `1M/jobs=12`, and that row has lower QAT byte
  share than interrupt, so some of the elapsed win can be workload/fallback mix.
- `5us` interval and quota `1` are promising for focused `1M` poll operation,
  but they were not validated across the full record/concurrency matrix.

Keep `zfs_qat_dc_poll=profile` effective interrupt for `balanced`,
`throughput`, `latency`, and `offload` until a broader matrix proves a safe
record/concurrency-independent default. Operators can still opt into polling
with the lock-step helper when testing larger-record throughput workloads.

## Final Host State

After testing, `pve.drewnet.online` was restored and rebooted:

```text
/etc/modprobe.d/zfs-qat.conf:
options zfs zfs_qat_compress_disable=0 zfs_qat_checksum_disable=1 zfs_qat_encrypt_disable=1 zfs_qat_cpa_dc_level=profile zfs_qat_dc_max_buf_size=profile

[KERNEL_QAT] Dc0..Dc5IsPolled = 0 on both DH895XCC configs
zfs_qat_dc_poll=profile
zfs_qat_dc_profile_recordsize=131072
zfs_qat_decompress_disable=profile
zfs_qat_dc_profile=balanced
dc_instances=12
dc_fails=0
dc_watchdog_health=1
```
