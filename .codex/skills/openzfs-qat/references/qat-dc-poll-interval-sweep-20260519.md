# QAT DC Poll Interval Sweep - 2026-05-19

## Purpose

Evaluate the permanent ZFS QAT DC polling option after adding init-time safety
validation. The test target was QAT 1.x `dh895xcc` hardware on
`pve.drewnet.online`.

Polling mode requires both sides to match:

- QAT driver `[KERNEL_QAT] Dc0IsPolled` through `Dc5IsPolled = 1`
- ZFS boot/module parameter `zfs_qat_dc_poll=1`

The code now queries each selected DC instance with `cpaDcInstanceGetInfo2()`
during QAT DC init and refuses QAT DC startup if the driver's `isPolled` value
does not match the effective `zfs_qat_dc_poll` mode.

## Safety Validation

Two boot states were validated:

| State | Driver DC mode | ZFS mode | Expected result | Observed result |
|---|---|---|---|---|
| Default IRQ | `DcNIsPolled=0` | `zfs_qat_dc_poll=profile` | QAT DC starts, no poller | Passed |
| Mismatch | `DcNIsPolled=0` | `zfs_qat_dc_poll=1` | QAT DC fails closed | Passed |
| Polling | `DcNIsPolled=1` | `zfs_qat_dc_poll=1` | QAT DC starts, poller runs | Passed |

The mismatch boot set `zfs_qat_compress_disable=1`, left `dc_instances=0`,
started no poller thread, and logged:

```text
QAT DC instance 0 poll mode mismatch: driver=interrupt zfs=poll
```

## Test Setup

- Host: `pve.drewnet.online`
- Cards: 2x `dh895xcc`
- Active DC instances: 12
- Kernel: `7.0.0-3-pve`
- ZFS srcversion: `138FD73E734ED60265387E2`
- QAT driver: DKMS `qat/4.28.0-00004`
- Source file: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`
- Source size: `191,346,108` bytes
- Harness: `/root/qat-phase4-benchmark.sh`
- Iterations: 3
- Verification: `VERIFY_MODE=sw`

The host was restored after testing to interrupt mode:

```text
zfs_qat_dc_poll=profile
zfs_qat_dc_profile_recordsize=131072
[KERNEL_QAT] Dc0IsPolled through Dc5IsPolled = 0
```

## 128K Interval Sweep

Lower elapsed time is better. `vs SW` compares QAT polling against software gzip
from the same rebuilt module and host state. Negative means QAT was faster.

| Interval us | Jobs | QAT ms | vs SW | QAT MiB/s | QAT CPU active | SW CPU active | Poll calls | Poll retries |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 | 818.573 | +15.80% | 224.5 | 1.89% | 4.19% | 276,684 | 275,436 |
| 1 | 1 | 790.463 | +11.83% | 231.5 | 1.68% | 4.19% | 32,532 | 31,291 |
| 5 | 1 | 756.180 | +6.98% | 241.9 | 1.76% | 4.19% | 27,016 | 25,775 |
| 10 | 1 | 711.410 | +0.64% | 257.7 | 1.83% | 4.19% | 21,204 | 19,960 |
| 25 | 1 | 804.179 | +13.77% | 227.6 | 1.71% | 4.19% | 14,024 | 12,799 |
| 50 | 1 | 743.999 | +5.25% | 245.7 | 1.68% | 4.19% | 8,592 | 7,385 |
| 0 | 4 | 1080.283 | -0.62% | 676.1 | 5.30% | 12.75% | 825,608 | 819,902 |
| 1 | 4 | 1068.993 | -1.66% | 683.4 | 5.44% | 12.75% | 99,880 | 94,233 |
| 5 | 4 | 1099.420 | +1.14% | 664.7 | 5.14% | 12.75% | 81,880 | 76,238 |
| 10 | 4 | 1114.847 | +2.56% | 655.4 | 4.96% | 12.75% | 66,448 | 60,826 |
| 25 | 4 | 1131.086 | +4.05% | 646.4 | 4.93% | 12.75% | 42,224 | 36,674 |
| 50 | 4 | 1113.990 | +2.48% | 657.1 | 4.99% | 12.75% | 26,524 | 21,062 |
| 0 | 8 | 1585.691 | +10.47% | 921.5 | 7.71% | 19.32% | 1,658,704 | 1,647,409 |
| 1 | 8 | 1535.131 | +6.95% | 951.4 | 7.87% | 19.32% | 204,408 | 193,238 |
| 5 | 8 | 1559.809 | +8.67% | 936.6 | 7.28% | 19.32% | 164,000 | 152,775 |
| 10 | 8 | 1478.973 | +3.03% | 987.4 | 7.60% | 19.32% | 131,948 | 120,723 |
| 25 | 8 | 1520.296 | +5.91% | 963.0 | 7.38% | 19.32% | 84,604 | 73,518 |
| 50 | 8 | 1575.200 | +9.74% | 926.9 | 7.27% | 19.32% | 53,224 | 42,298 |

Text chart, elapsed time at 128K:

```text
Jobs 1: SW 706.874 | 10us 711.410 | 50us 743.999 | 5us 756.180 | 1us 790.463 | 25us 804.179 | 0us 818.573
Jobs 4: 1us 1068.993 | 0us 1080.283 | SW 1087.040 | 5us 1099.420 | 50us 1113.990 | 10us 1114.847 | 25us 1131.086
Jobs 8: SW 1435.421 | 10us 1478.973 | 25us 1520.296 | 1us 1535.131 | 5us 1559.809 | 50us 1575.200 | 0us 1585.691
```

## Larger Records At 10 us

The first larger-record pass was invalid because the host still had effective
`zfs_qat_dc_max_buf_size=131072`, so `256K+` rows fell back to software. The
valid pass below booted with `zfs_qat_dc_profile_recordsize=1048576`, making
`zfs_qat_dc_max_buf_size=profile` effective at `1M`.

| Record | Jobs | QAT ms | SW ms | vs SW | QAT CPU active | SW CPU active | Ratio | QAT requests |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 256K | 1 | 694.732 | 699.584 | -0.69% | 1.63% | 4.23% | 21.63x | 730 |
| 256K | 4 | 1035.483 | 1118.667 | -7.44% | 4.57% | 16.89% | 21.70x | 2,920 |
| 256K | 8 | 1453.873 | 1401.676 | +3.72% | 6.97% | 20.10% | 21.71x | 5,846 |
| 512K | 1 | 635.224 | 581.767 | +9.19% | 1.58% | 4.71% | 24.49x | 365 |
| 512K | 4 | 1037.480 | 1033.076 | +0.43% | 4.22% | 14.24% | 24.58x | 1,460 |
| 512K | 8 | 1472.558 | 1484.800 | -0.82% | 6.38% | 21.72% | 24.60x | 2,927 |
| 1M | 1 | 575.707 | 590.202 | -2.46% | 1.67% | 4.78% | 25.15x | 183 |
| 1M | 4 | 1040.617 | 989.613 | +5.15% | 3.92% | 12.87% | 25.25x | 732 |
| 1M | 8 | 1575.866 | 1644.091 | -4.15% | 6.45% | 22.13% | 25.25x | 1,472 |

Text chart, larger-record elapsed time at 10 us:

```text
256K jobs 1: QAT 694.732 < SW 699.584
256K jobs 4: QAT 1035.483 < SW 1118.667
256K jobs 8: SW 1401.676 < QAT 1453.873
512K jobs 1: SW 581.767 < QAT 635.224
512K jobs 4: SW 1033.076 ~= QAT 1037.480
512K jobs 8: QAT 1472.558 < SW 1484.800
1M jobs 1: QAT 575.707 < SW 590.202
1M jobs 4: SW 989.613 < QAT 1040.617
1M jobs 8: QAT 1575.866 < SW 1644.091
```

## Interpretation

- Polling mode is now safe enough to keep as a permanent operator-visible
  option because mode mismatches fail closed at QAT DC init.
- `10 us` is the best 128K compromise in this sweep. It is best at `JOBS=1`
  and `JOBS=8`; `1 us` is slightly better only at `JOBS=4`.
- Busy polling with interval `0` is not attractive: it has very high retry
  counts and no consistent elapsed-time win.
- QAT polling continues to provide a large active-CPU reduction even when
  elapsed time loses to software gzip.
- Larger records can use QAT only when the effective max buffer/profile
  recordsize is raised. With `zfs_qat_dc_profile_recordsize=1048576`, the QAT
  path handled `256K`, `512K`, and `1M` records.
- Larger records do not eliminate the response-wait issue. QAT wins some
  elapsed-time cases, but not all, and driver wait per MiB remains in the same
  broad range as the earlier interrupt and polling observations.

## Artifacts

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-irq-smoke-after-poll-validation-128k-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-restored-irq-final-smoke-128k-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval0-default128k-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval0-default128k-jobs4-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval0-default128k-jobs8-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval1-default128k-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval1-default128k-jobs4-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval1-default128k-jobs8-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval5-default128k-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval5-default128k-jobs4-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval5-default128k-jobs8-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval10-default128k-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval10-default128k-jobs4-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval10-default128k-jobs8-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval25-default128k-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval25-default128k-jobs4-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval25-default128k-jobs8-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval50-default128k-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval50-default128k-jobs4-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval50-default128k-jobs8-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-sw-pollconfig-default128k-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-sw-pollconfig-default128k-jobs4-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-sw-pollconfig-default128k-jobs8-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval10-large-records-valid-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval10-large-records-valid-jobs4-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-interval10-large-records-valid-jobs8-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-sw-large-records-validconfig-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-sw-large-records-validconfig-jobs4-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-sw-large-records-validconfig-jobs8-20260519.csv
```
