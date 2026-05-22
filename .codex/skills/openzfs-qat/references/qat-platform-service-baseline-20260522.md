# QAT Platform And Service Baseline - 2026-05-22

Purpose: capture the current `pve.drewnet.online` QAT platform state and test
whether service-level QAT configuration can reduce compression cost for the
current best `1M` throughput benchmark.

## Host State

Host: `pve.drewnet.online`

ZFS module:

```text
filename: /lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko
version: 2.4.99-1
srcversion: 38C562CCF5F59E6889FDE50
```

QAT hardware:

| Device | Type | PCI BDF | NUMA node | Local CPUs | Link capability | Link state |
|---|---|---|---:|---|---|---|
| `qat_dev0` | `dh895xcc` | `0000:45:00.0` | 2 | `16-23,48-55` | Gen2 x16 | Gen2 x16 |
| `qat_dev1` | `dh895xcc` | `0000:64:00.0` | 3 | `24-31,56-63` | Gen2 x16 | Gen2 x16 |

The PCIe links are trained at the devices' reported capability. This baseline
does not show an obvious PCIe lane-width or link-speed bottleneck.

QAT/ZFS state after final restore:

```text
zfs_qat_dc_profile=balanced
zfs_qat_dc_profile_recordsize=131072
zfs_qat_dc_max_buf_size=profile
zfs_qat_dc_coalesce_src=profile
zfs_qat_dc_coalesce_dst=profile
zfs_qat_decompress_disable=profile
zfs_qat_dc_poll=profile
zfs_qat_compress_disable=0
zfs_qat_checksum_disable=1
zfs_qat_encrypt_disable=1
zfs_qat_cpa_dc_level=profile
dc_instances=12
dc_watchdog_health=1
qat.service=active
```

Artifacts:

```text
.codex/skills/openzfs-qat/artifacts/qat-platform-baseline-20260522.txt
.codex/skills/openzfs-qat/artifacts/qat-service-dc-only-failure-20260522.log
.codex/skills/openzfs-qat/artifacts/zfs-qat-service-cydc-throughput-1m-jobs4-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-service-cydc-throughput-1m-jobs8-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-service-cydc-kernelcy0-throughput-1m-jobs4-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-service-cydc-kernelcy0-throughput-1m-jobs8-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-service-split-summary-20260522.csv
```

## Service Configuration Findings

Baseline config before this experiment:

```text
[GENERAL]
ServicesEnabled = cy;dc

[KERNEL]
NumberCyInstances = 1
NumberDcInstances = 0
Cy0IsPolled = 0

[KERNEL_QAT]
NumberCyInstances = 0
NumberDcInstances = 6
Dc0..Dc5IsPolled = 0
```

This means the ZFS kernel QAT client was already using DC-only instances through
`[KERNEL_QAT]`. ZFS saw `dc_instances=12` across the two cards.

An attempted full DC-only service configuration failed:

```text
[GENERAL]
ServicesEnabled = dc
```

With `ServicesEnabled=dc`, `qat.service` completed but both devices were down.
The service log showed `Ioctl failed`, `Failed to load config data to device`,
and `Failed to configure qat_dev0/qat_dev1`.

Decision:

- Do not use `ServicesEnabled=dc` on this QAT 4.28/dh895xcc deployment.
- Keep `ServicesEnabled=cy;dc` even when the kernel QAT client is DC-only.
- `[KERNEL] NumberCyInstances=0` is viable with `ServicesEnabled=cy;dc`, but it
  is not proven as a broad performance win by this benchmark.

## Benchmark

Temporary benchmark boot setting:

```text
zfs_qat_dc_profile_recordsize=1048576
zfs_qat_decompress_disable=1
```

Runtime setting during QAT rows:

```text
zfs_qat_dc_profile=throughput
```

The host was restored to normal default/profile ZFS QAT boot settings after the
benchmark.

Summary from raw rows:

| Variant | Jobs | Mode | Avg elapsed ms | QAT vs SW elapsed | Avg CPU active s/GiB | Avg QAT byte share |
|---|---:|---|---:|---:|---:|---:|
| `cy;dc`, `[KERNEL] cy=1` | 4 | QAT | 929.197 | 4.06% faster | 5.496 | 79.94% |
| `cy;dc`, `[KERNEL] cy=1` | 4 | SW | 968.549 | baseline | 11.817 | 0.00% |
| `cy;dc`, `[KERNEL] cy=1` | 8 | QAT | 1243.771 | 16.15% faster | 5.912 | 75.80% |
| `cy;dc`, `[KERNEL] cy=1` | 8 | SW | 1483.295 | baseline | 13.452 | 0.00% |
| `cy;dc`, `[KERNEL] cy=0` | 4 | QAT | 906.954 | 5.34% faster | 5.090 | 90.29% |
| `cy;dc`, `[KERNEL] cy=0` | 4 | SW | 958.131 | baseline | 12.561 | 0.00% |
| `cy;dc`, `[KERNEL] cy=0` | 8 | QAT | 1296.981 | 9.70% faster | 6.477 | 76.82% |
| `cy;dc`, `[KERNEL] cy=0` | 8 | SW | 1436.230 | baseline | 12.750 | 0.00% |

Direct QAT-to-QAT comparison:

| Change | Jobs | QAT elapsed change |
|---|---:|---:|
| `[KERNEL] NumberCyInstances=1` to `0` | 4 | 2.39% faster |
| `[KERNEL] NumberCyInstances=1` to `0` | 8 | 4.28% slower |

Interpretation:

- The viable service split did not produce a consistent elapsed-time win.
- QAT continues to reduce CPU cost materially versus software gzip on this
  workload.
- The result is not strong enough to promote `[KERNEL] NumberCyInstances=0` as
  a performance default based only on elapsed time.
- Leaving `[KERNEL] NumberCyInstances=0` is still reasonable for this dedicated
  compression test host because ZFS checksum and encryption offload are disabled
  and `[KERNEL_QAT]` remains DC-only.

## QAT Driver Parameter Checking

The QAT source tree used on the host enables parameter checking by default:

```text
quickassist/Makefile: export ICP_PARAM_CHECK ?= y
quickassist/build_system/build_files/common.mk: EXTRA_CFLAGS+=-DICP_PARAM_CHECK
```

The next QAT-side experiment should rebuild the QAT DKMS package with
`ICP_PARAM_CHECK=n`, then repeat the same `1M` throughput benchmark with
correctness checks. Treat this as a controlled experiment, not a default change,
until it proves both correctness and elapsed-time benefit.

## Next Target

Proceed to the QAT driver parameter-checking experiment before adding more
ZFS-side policy. The platform/service baseline has ruled out PCIe link width and
simple service-split changes as obvious remaining bottlenecks.
