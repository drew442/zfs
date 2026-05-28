# QAT Admission Attribution - 2026-05-28

## Purpose

Improve benchmark interpretation so QAT-mode wins can be separated into:

- QAT engine work.
- Profile/admission fallback caused by size policy.
- Runtime-disabled or uninitialized fallback.
- Async/resource fallback after admission.

This is a harness and documentation change only. It does not change kernel
policy, profile behavior, QAT eligibility, fallback behavior, or compressed
data format.

## Harness Change

`scripts/qat-phase4-benchmark.sh` now appends these derived columns:

- `qat_eligible_share_pct`: `dc_compress_accel_eligible_delta /
  dc_compress_accel_checks_delta`.
- `qat_profile_skip_share_pct`: `(dc_compress_accel_skip_min_delta +
  dc_compress_accel_skip_max_delta) /
  dc_compress_accel_checks_delta`.
- `qat_runtime_skip_share_pct`: `(dc_compress_accel_skip_disabled_delta +
  dc_compress_accel_skip_runtime_delta +
  dc_compress_accel_skip_uninit_delta) /
  dc_compress_accel_checks_delta`.
- `qat_async_failure_share_pct`: `(dc_compress_async_submit_fails_delta +
  dc_compress_async_fail_retry_delta +
  dc_compress_async_fail_resource_delta +
  dc_compress_async_fail_other_delta) /
  dc_compress_async_submits_delta`.

These are derived from counters already captured in each raw CSV row.

## Important Limitation

The eligibility counters follow `zfs_qat_dc_shape_stats`.

With `zfs_qat_dc_shape_stats=profile`, the effective balanced/default value is
`0`, so these derived attribution fields are expected to be `na`. That means
normal low-overhead benchmark runs still report QAT byte share and async
fallback/cap-skip share, but not detailed eligibility attribution.

Set `zfs_qat_dc_shape_stats=1` for focused admission-policy runs that need to
distinguish size-policy skips from runtime or async/resource fallback.

## Validation

Host:

- `pve.drewnet.online`
- Kernel: `7.0.0-3-pve`
- Loaded ZFS `srcversion`: `AD7B8686FC9E2032979E83E`
- Profile: `balanced`
- Profile target record size: `131072`
- Active QAT DC instances visible to ZFS: `12`
- Post-test `zfs_qat_dc_shape_stats`: `profile`
- `zpool status -x`: all pools healthy
- `dc_fails`: `0`
- `dc_watchdog_health`: `1`

Artifacts:

- `artifacts/admission-attribution-20260528/hdd-admission-smoke.csv`
- `artifacts/admission-attribution-20260528/nvme-admission-smoke.csv`
- `artifacts/admission-attribution-20260528/hdd-admission-shape1-smoke.csv`
- `artifacts/admission-attribution-20260528/nvme-admission-shape1-smoke.csv`
- `artifacts/admission-attribution-20260528/admission-attribution-summary.csv`

Smoke matrix:

- Media: HDD and NVMe
- Records: `128K` and `1M`
- Mode: `qat`
- Iterations: `1`
- Jobs: `1`
- Verification: software readback comparison

All raw CSVs have `267` columns. All raw rows reported `sha_ok=yes` and
`dc_fails_delta=0`.

## Result

| media | shape stats | record | accel checks | eligible share | profile skip share |
| --- | --- | ---: | ---: | ---: | ---: |
| HDD | `1` | 128K | 1460 | 100.00% | 0.00% |
| HDD | `1` | 1M | 183 | 0.00% | 100.00% |
| HDD | `profile` | 128K | 0 | na | na |
| HDD | `profile` | 1M | 0 | na | na |
| NVMe | `1` | 128K | 1460 | 100.00% | 0.00% |
| NVMe | `1` | 1M | 183 | 0.00% | 100.00% |
| NVMe | `profile` | 128K | 0 | na | na |
| NVMe | `profile` | 1M | 0 | na | na |

Under the default balanced/128K profile, `1M` is not an async/resource problem.
When attribution is enabled, it is classified as a profile-size skip because the
effective maximum QAT DC buffer size is `128K`.

## Interpretation

Use this split in future policy decisions:

- `qat_profile_skip_share_pct` answers whether the selected profile intentionally
  excluded records by size.
- `qat_runtime_skip_share_pct` answers whether QAT was disabled, uninitialized,
  or runtime-failed before admission.
- `qat_cap_skip_share_pct` answers whether async cap policy chose software after
  the request was otherwise eligible for the async path.
- `qat_async_failure_share_pct` answers whether submit failures or async
  failure classes forced fallback.

This makes hybrid-policy wins easier to classify. A run with high profile-skip
share is an admission-policy result; a run with high cap-skip or async-failure
share is a pressure/resource result; a run with high QAT byte share and stable
or better elapsed time is closer to a QAT engine improvement.
