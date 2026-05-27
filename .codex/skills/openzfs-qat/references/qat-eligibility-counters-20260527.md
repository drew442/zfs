# QAT compression eligibility counters

Date: 2026-05-27

## Purpose

The benchmark harness could already report QAT completions, software fallbacks,
and async-cap skips, but it could not directly explain why a record was never
submitted to QAT. That made it easy to confuse a faster software-fallback result
with a QAT improvement.

This pass adds diagnostic kstats for the compression acceleration decision:

- `dc_compress_accel_checks`
- `dc_compress_accel_eligible`
- `dc_compress_accel_skip_disabled`
- `dc_compress_accel_skip_runtime`
- `dc_compress_accel_skip_uninit`
- `dc_compress_accel_skip_min`
- `dc_compress_accel_skip_max`

These counters are updated only when effective detailed shape stats are enabled.
They count calls to `qat_dc_compress_use_accel()`, not logical ZFS records. The
async path can check eligibility more than once per logical record, so these
counters should be used for attribution, not as request totals.

## Files changed

- `include/sys/qat.h`: added kstat fields.
- `module/os/linux/zfs/qat.c`: registered the new kstat names.
- `module/os/linux/zfs/qat_compress.c`: increments the counters behind
  `qat_dc_effective_shape_stats()`.
- `.codex/skills/openzfs-qat/scripts/qat-phase4-benchmark.sh`: captures the new
  counter deltas in CSV output.

## Validation

Local checks:

```sh
bash -n .codex/skills/openzfs-qat/scripts/qat-phase4-benchmark.sh
git diff --check
```

Host checks on `pve.drewnet.online`:

- Rebuilt and installed ZFS DKMS for `7.0.0-3-pve` against
  `/usr/src/qat-4.28.0-00004`.
- Updated initramfs and rebooted.
- Verified loaded and installed ZFS `srcversion` match:
  `FD3391E9A3B4CC84F177BA7`.
- Verified QAT active with two DH895XCC cards and `12` DC instances.
- Verified pools healthy, `dc_fails=0`, `dc_watchdog_health=1`.
- Verified host restored to:
  `zfs_qat_dc_profile=balanced`,
  `zfs_qat_dc_profile_recordsize=131072`,
  `zfs_qat_dc_shape_stats=profile`.

Artifacts:

- `artifacts/eligibility-counters-20260527/qat-eligibility-counters-test-hdd-pool-20260527-211213.csv`
- `artifacts/eligibility-counters-20260527/qat-eligibility-counters-nvme_scratch-20260527-211228.csv`
- `artifacts/eligibility-counters-20260527/qat-eligibility-throughput512-test-hdd-pool-20260527-211732.csv`
- `artifacts/eligibility-counters-20260527/qat-eligibility-throughput512-nvme_scratch-20260527-211742.csv`

## Default balanced profile

Shape stats were forced on for attribution. The boot profile was otherwise the
default balanced profile with `128K` profile record size.

| media | mode | record | elapsed ms | MiB/s | CPU active % | QAT byte share % | checks | eligible | disabled skip | max skip | cap skips |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| HDD | QAT | 128K | 1172.727 | 622.42 | 3.83 | 100.00 | 5840 | 5840 | 0 | 0 | 0 |
| HDD | SW | 128K | 1083.196 | 673.86 | 13.54 | 0.00 | 5840 | 0 | 5840 | 0 | 0 |
| HDD | QAT | 512K | 1010.323 | 722.47 | 13.21 | 0.00 | 1460 | 0 | 0 | 1460 | 0 |
| HDD | SW | 512K | 1019.223 | 716.16 | 12.58 | 0.00 | 1460 | 0 | 1460 | 0 | 0 |
| HDD | QAT | 1M | 907.538 | 804.29 | 13.85 | 0.00 | 732 | 0 | 0 | 732 | 0 |
| HDD | SW | 1M | 971.023 | 751.71 | 13.51 | 0.00 | 732 | 0 | 732 | 0 | 0 |
| NVMe | QAT | 128K | 933.493 | 781.93 | 3.49 | 100.00 | 5840 | 5840 | 0 | 0 | 0 |
| NVMe | SW | 128K | 777.326 | 939.02 | 15.86 | 0.00 | 5840 | 0 | 5840 | 0 | 0 |
| NVMe | QAT | 512K | 756.910 | 964.35 | 16.53 | 0.00 | 1460 | 0 | 0 | 1460 | 0 |
| NVMe | SW | 512K | 754.532 | 967.39 | 16.58 | 0.00 | 1460 | 0 | 1460 | 0 | 0 |
| NVMe | QAT | 1M | 728.226 | 1002.34 | 17.48 | 0.00 | 732 | 0 | 0 | 732 | 0 |
| NVMe | SW | 1M | 737.307 | 989.99 | 17.12 | 0.00 | 732 | 0 | 732 | 0 | 0 |

Interpretation:

- `128K` QAT rows are genuinely QAT-eligible.
- `512K` and `1M` QAT rows in the default profile are software fallback due to
  the profile max, not async-cap pressure and not QAT engine behavior.
- Software rows show `skip_disabled`, as expected.

## Temporary throughput 512K profile

The throughput 512K diagnostic required a temporary boot profile because
`zfs_qat_dc_profile_recordsize` and `zfs_qat_dc_max_buf_size` rejected runtime
changes with `EBUSY`.

Temporary boot values:

```text
zfs_qat_dc_profile=throughput
zfs_qat_dc_profile_recordsize=524288
zfs_qat_dc_shape_stats=1
```

The host was restored to balanced/profile after the run.

| media | mode | record | elapsed ms | MiB/s | CPU active % | QAT byte share % | checks | eligible | disabled skip | max skip | cap skips | QAT completions | async fallbacks |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| HDD | QAT | 512K | 1046.248 | 697.66 | 6.27 | 60.42 | 2920 | 2920 | 0 | 0 | 578 | 882 | 578 |
| HDD | SW | 512K | 978.591 | 745.90 | 13.14 | 0.00 | 2920 | 0 | 2920 | 0 | 0 | 0 | 0 |
| HDD | QAT | 1M | 1006.392 | 725.29 | 12.70 | 0.00 | 1464 | 0 | 0 | 1464 | 0 | 0 | 0 |
| HDD | SW | 1M | 967.021 | 754.82 | 13.50 | 0.00 | 1464 | 0 | 1464 | 0 | 0 | 0 | 0 |
| NVMe | QAT | 512K | 752.879 | 969.51 | 9.66 | 47.88 | 2920 | 2920 | 0 | 0 | 761 | 699 | 761 |
| NVMe | SW | 512K | 762.972 | 956.69 | 17.10 | 0.00 | 2920 | 0 | 2920 | 0 | 0 | 0 | 0 |
| NVMe | QAT | 1M | 762.864 | 956.83 | 16.64 | 0.00 | 1464 | 0 | 0 | 1464 | 0 | 0 | 0 |
| NVMe | SW | 1M | 759.483 | 961.08 | 16.87 | 0.00 | 1464 | 0 | 1464 | 0 | 0 | 0 | 0 |

Interpretation:

- `512K` under throughput/512K is eligible, but the QAT byte share is only
  `60.42%` on HDD and `47.88%` on NVMe because async-cap fallback is active.
- `1M` under throughput/512K is not eligible and falls back due to max-size
  policy. It is not an async-cap case.
- The HDD `512K` QAT row saved CPU but was slower than software in this single
  iteration. The NVMe `512K` QAT row was slightly faster than software and used
  materially less CPU.

## Implication for next policy work

Future profile decisions should not use elapsed time alone. At minimum, compare:

- elapsed throughput and latency,
- CPU active seconds per GiB,
- QAT byte share,
- eligibility skip reason,
- async-cap skip rate,
- compression ratio.

The new counters make it practical to reject policy changes that look faster
only because QAT was bypassed. They also show whether to tune min/max profile
policy or async-cap/resource policy for a given record size.
