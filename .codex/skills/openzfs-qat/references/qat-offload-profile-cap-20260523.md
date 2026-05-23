# QAT Offload Profile Cap Policy - 2026-05-23

Purpose: separate the `offload` profile from the `throughput` profile so CPU
offload can be chosen explicitly without changing the throughput default.

## Change

The async cap policy now has three internal profile modes:

```text
balanced: existing record-size caps
throughput: existing 1M throughput cap behavior
offload: higher cap for 512K and larger records
```

The operator-facing default is unchanged:

```text
zfs_qat_dc_profile=balanced
zfs_qat_dc_async_cap_policy=profile
```

When `zfs_qat_dc_profile=offload` and
`zfs_qat_dc_profile_recordsize >= 512K`, profile cap selection now allows all
active DC instances and uses a larger per-instance cap for `512K+` records.
This is intended to reduce host CPU use, not to replace the throughput profile.

## Boot Ordering Fix

During validation, a stale host configuration reintroduced a systemd ordering
cycle:

```text
zfs-import-cache.service/zfs-mount.service -> qat.service -> local-fs.target
```

When systemd broke the cycle, `qat.service` did not run and ZFS initially had
`dc_instances=0`. Starting `qat.service` and running `zfs-qat-reenable.service`
restored `dc_instances=12`.

The host was repaired by removing:

```text
/etc/systemd/system/zfs-import-cache.service.d/qat.conf
/etc/systemd/system/zfs-mount.service.d/qat.conf
```

`contrib/qat/install-zfs-qat-reenable.sh` now removes those obsolete drop-ins
when installing the re-enable unit.

## Validation

Build and boot validation:

```text
kernel: 7.0.0-3-pve
zfs srcversion: 1FFFD32E6969A944BE114F1
dc_instances=12
dc_fails=0
```

Focused cap sweep before the code change:

```text
records: 128K, 512K, 1M
jobs: 4, 8
iterations: 3
modes: qat
profile: throughput
record target: 1M
caps: profile, fixed 96, fixed 192, fixed 384
```

Offload-profile validation after the code change:

```text
records: 512K, 1M
jobs: 4, 8
iterations: 3
modes: qat
profile: offload
record target: 1M
```

Artifacts:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-cap-sweep-post-scratch-summary-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-offload-profile-cap-summary-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-cap-sweep-post-scratch-cap*-records128k512k1m-jobs*-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-offload-profile-cap-records512k1m-jobs*-20260523.csv
.codex/skills/openzfs-qat/artifacts/host-root-import-20260523/zfs-qat-cap-sweep-post-scratch-20260523-run.log
.codex/skills/openzfs-qat/artifacts/host-root-import-20260523/zfs-qat-offload-profile-cap-20260523-run.log
```

## Results

Offload profile after the change:

| Record | Jobs | Elapsed ms | QAT share | Fallback share | CPU s/GiB | QAT service ns/MiB |
|---|---:|---:|---:|---:|---:|---:|
| 512K | 4 | 942.8 | 70.4% | 29.6% | 6.35 | 64,018,908 |
| 1M | 4 | 921.6 | 96.3% | 4.0% | 4.01 | 49,228,175 |
| 512K | 8 | 1199.7 | 70.0% | 30.2% | 6.40 | 70,128,540 |
| 1M | 8 | 1345.5 | 89.5% | 11.2% | 4.87 | 45,025,506 |

Compared with the throughput full-matrix rows from
`qat-scratch-reuse-fullmatrix-20260523.md`, offload profile uses more QAT and
less CPU at a small elapsed-time cost:

| Record | Jobs | Throughput ms | Offload ms | Throughput CPU s/GiB | Offload CPU s/GiB |
|---|---:|---:|---:|---:|---:|
| 512K | 4 | 906.6 | 942.8 | 7.68 | 6.35 |
| 1M | 4 | 910.6 | 921.6 | 6.00 | 4.01 |
| 512K | 8 | 1176.0 | 1199.7 | 7.50 | 6.40 |
| 1M | 8 | 1283.8 | 1345.5 | 5.98 | 4.87 |

## Decision

Keep throughput profile unchanged. The cap sweep did not show a safe elapsed
throughput win from increasing the `512K` cap.

Use the new behavior only for `offload`. It gives the operator an explicit CPU
reduction bias while preserving the existing throughput and balanced behavior.

Next tuning should target latency/elapsed improvements separately, because the
offload profile intentionally accepts more QAT queueing to reduce CPU work.
