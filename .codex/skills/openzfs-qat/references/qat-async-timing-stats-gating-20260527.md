# QAT Async Timing-Stats Gating - 2026-05-27

## Purpose

Reduce async compression request overhead by making detailed local timing
instrumentation opt-in instead of always-on.

This is a request-overhead change, not a compression policy change. It does not
change QAT eligibility, fallback, watchdog timing, inflight accounting, callback
completion handling, compression ratio, or data format.

## Change

Added `zfs_qat_dc_timing_stats`, with accepted values:

- `profile`: default, currently resolves to `0`.
- `0`: disable detailed async compression local timing stats.
- `1`: enable detailed async compression local timing stats for benchmarking.

When disabled, the async compression path skips nonessential `gethrtime()` calls
and timing-stat atomics for:

- async request allocation/free timing
- page-array allocation/free timing
- buffer-list allocation/free timing
- scratch allocation/free timing
- async compression setup timing
- async cleanup timing
- source and destination coalesce free timing

Correctness-critical timestamps remain active, including async submit completion,
inflight tracking, callback completion, watchdog progress, and timeout recovery.

The benchmark harness now records both `zfs_qat_dc_timing_stats` and
`zfs_qat_dc_effective_timing_stats` so future runs show whether detailed local
request timing was enabled.

## Media-Bias Follow-Up

The work plan now explicitly calls out a later full sweep for media-bias
profiles. That sweep must test every current QAT profile-driven setting and
manual tunable individually across HDD and NVMe, not only the settings already
suspected of being media-sensitive.

The goal is to identify all settings whose best value changes by storage media
class before adding `rotational`, `flash`, or similar profile inputs.

## Validation

Host: `pve.drewnet.online`

Kernel: `7.0.0-3-pve`

QAT hardware: two DH895XCC cards, `12` DC instances

ZFS module `srcversion`: `EECE0B490A3AF51BC95F2BC`

Test profile:

- `zfs_qat_dc_profile=throughput`
- `zfs_qat_dc_profile_recordsize=1048576`
- `zfs_qat_dc_async=profile`
- `zfs_qat_dc_async_max_inflight=profile`
- `zfs_qat_dc_async_cap_policy=profile`
- `zfs_qat_dc_shape_stats=profile`
- `MODES=qat`
- `RECORDS="512K 1M"`
- `JOBS=4`
- `ITERS=3`

Artifacts:

- `artifacts/async-timing-gate-20260527/hdd-timing-profile.csv`
- `artifacts/async-timing-gate-20260527/hdd-timing-1.csv`
- `artifacts/async-timing-gate-20260527/nvme-timing-profile.csv`
- `artifacts/async-timing-gate-20260527/nvme-timing-1.csv`

Summary averages from raw rows:

| media | record | timing stats | elapsed ms | MiB/s | CPU active % | QAT byte share % | QAT wait ns/req | setup ns/req |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| HDD | 512K | off (`profile`) | 957.835 | 762.410 | 7.123 | 52.700 | 9,932,148 | 0 |
| HDD | 512K | on (`1`) | 946.339 | 778.053 | 7.713 | 47.127 | 10,060,806 | 21,929 |
| HDD | 1M | off (`profile`) | 854.017 | 855.990 | 5.817 | 73.207 | 32,782,818 | 0 |
| HDD | 1M | on (`1`) | 847.445 | 861.360 | 5.740 | 72.567 | 33,444,234 | 36,436 |
| NVMe | 512K | off (`profile`) | 718.783 | 1,016.317 | 9.850 | 46.167 | 10,054,159 | 0 |
| NVMe | 512K | on (`1`) | 738.953 | 987.880 | 10.040 | 46.167 | 10,025,100 | 24,481 |
| NVMe | 1M | off (`profile`) | 700.841 | 1,041.610 | 6.767 | 72.017 | 32,790,529 | 0 |
| NVMe | 1M | on (`1`) | 745.238 | 980.163 | 6.123 | 77.313 | 32,031,100 | 38,013 |

## Result

The new parameter works as intended:

- With `profile`, detailed async setup/cleanup timing counters remain zero.
- With `1`, detailed timing counters populate and show local setup measurement
  around `22-38 us/request` in this test.
- QAT correctness checks passed in every raw row.
- `dc_fails` stayed at `0`.
- Pools were restored to the normal balanced `128K` boot profile after testing.

The wall-clock result is mixed. Timing-stats-off was clearly faster on NVMe in
this run, while HDD results were within noise and had different QAT/software
share. The change should still remain because it removes known per-request
diagnostic work from the normal path while preserving detailed observability
when explicitly enabled.

## Next Request-Overhead Target

Continue with request-shape and mapping overhead. The remaining large costs are
still dominated by per-request page mapping, buffer-list construction, QAT wait
time, and hybrid fallback behavior, not by the now-gated local timing counters.
