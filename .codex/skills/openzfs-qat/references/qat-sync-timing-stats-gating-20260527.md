# QAT Sync Timing-Stats Gating - 2026-05-27

## Purpose

Continue request-overhead reduction by extending `zfs_qat_dc_timing_stats`
beyond the async path. The default `balanced` profile accelerates `128K` records
through the synchronous compression path, so leaving sync timing always enabled
kept unnecessary timestamp/stat work on the normal profile.

## Change

`zfs_qat_dc_timing_stats=profile` now disables detailed local compression
timing for both sync and async QAT compression requests.

The sync compression path now skips detailed timing for:

- compression-bound timing
- scratch allocation/free timing
- page-array allocation/free timing
- buffer-list allocation/free timing
- sync request allocation/free timing
- sync compression setup timing
- sync compression submit timing
- sync compression cleanup timing
- source/destination coalesce cleanup timing

Correctness and failure behavior are unchanged. QAT wait/inflight timing remains
active because callback and inflight progress still require real timestamps.
Decompression timing is left unchanged by this pass.

## Validation

Host: `pve.drewnet.online`

Kernel: `7.0.0-3-pve`

QAT hardware: two DH895XCC cards, `12` DC instances

ZFS module `srcversion`: `F461C0BF1A7F72B23F8E54E`

Artifacts:

- `artifacts/sync-timing-gate-20260527/default-hdd-timing-profile.csv`
- `artifacts/sync-timing-gate-20260527/default-hdd-timing-1.csv`
- `artifacts/sync-timing-gate-20260527/default-nvme-timing-profile.csv`
- `artifacts/sync-timing-gate-20260527/default-nvme-timing-1.csv`
- `artifacts/sync-timing-gate-20260527/throughput1m-hdd-timing-profile.csv`
- `artifacts/sync-timing-gate-20260527/throughput1m-hdd-timing-1.csv`
- `artifacts/sync-timing-gate-20260527/throughput1m-nvme-timing-profile.csv`
- `artifacts/sync-timing-gate-20260527/throughput1m-nvme-timing-1.csv`
- `artifacts/sync-timing-gate-20260527/summary.csv`

All measurement rows:

- `sha_ok=yes`
- `dc_fails_delta=0`
- raw CSV files have consistent `263`-column width; blank separator rows are
  present between record-size groups

The host was restored after testing:

- `zfs_qat_dc_profile=balanced`
- `zfs_qat_dc_profile_recordsize=131072`
- `zfs_qat_dc_timing_stats=profile`
- pools online
- `dc_fails=0`
- `dc_watchdog_health=1`

## Result Summary

Default balanced profile, `128K` sync compression:

| media | timing stats | elapsed ms | MiB/s | CPU active % | setup ns/req | submit ns/req | cleanup ns/req | wait ns/req |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| HDD | off (`profile`) | 1150.082 | 634.957 | 3.810 | 0 | 0 | 0 | 1,337,923 |
| HDD | on (`1`) | 1130.126 | 646.287 | 3.803 | 5,983 | 8,005 | 297 | 1,344,141 |
| NVMe | off (`profile`) | 942.139 | 774.837 | 3.417 | 0 | 0 | 0 | 1,270,057 |
| NVMe | on (`1`) | 961.689 | 759.190 | 3.293 | 9,562 | 10,295 | 291 | 1,364,840 |

Throughput/1M profile, accelerated larger-record coverage:

| media | record | timing stats | elapsed ms | MiB/s | CPU active % | QAT byte share % | setup ns/req | submit ns/req | cleanup ns/req | wait ns/req |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| HDD | 512K | off (`profile`) | 903.298 | 809.080 | 7.970 | 52.107 | 0 | 21,942 | 0 | 9,981,573 |
| HDD | 512K | on (`1`) | 943.721 | 777.260 | 7.783 | 47.240 | 21,817 | 22,260 | 131 | 10,104,958 |
| HDD | 1M | off (`profile`) | 862.997 | 847.617 | 5.820 | 73.800 | 0 | 38,818 | 0 | 32,939,071 |
| HDD | 1M | on (`1`) | 862.175 | 847.200 | 5.797 | 72.337 | 37,758 | 38,686 | 140 | 33,453,687 |
| NVMe | 512K | off (`profile`) | 718.628 | 1016.783 | 9.810 | 45.233 | 0 | 22,014 | 0 | 10,051,886 |
| NVMe | 512K | on (`1`) | 713.042 | 1024.727 | 10.063 | 45.277 | 23,821 | 21,917 | 129 | 10,024,829 |
| NVMe | 1M | off (`profile`) | 706.273 | 1033.527 | 6.260 | 73.523 | 0 | 38,062 | 0 | 32,815,285 |
| NVMe | 1M | on (`1`) | 726.717 | 1004.443 | 6.250 | 75.717 | 38,431 | 38,667 | 153 | 31,836,201 |

## Interpretation

The structural behavior is correct: timing-off rows zero the newly gated local
sync timing counters while QAT wait timing, correctness checks, and failure
counters remain active.

End-to-end elapsed results are mixed, which is expected at this scale:

- Default `128K` sync timing-off was faster on NVMe and slower on HDD.
- Throughput/1M timing-off was faster on HDD `512K` and NVMe `1M`, effectively
  flat on HDD `1M`, and slower on NVMe `512K`.
- QAT byte share changed between some paired rows, so elapsed differences should
  not be treated as pure timing-instrumentation cost.

Keep the change because it removes known diagnostic timestamp/stat overhead from
the default compression path while preserving opt-in visibility for benchmark
runs. It does not materially change policy or data-path correctness.

## Next Request-Overhead Target

The remaining large latency term is still QAT wait/service time, not local
setup. Further request-overhead work should focus on changes that can improve
QAT share, reduce cap/fallback churn, or reduce request count without adding
copy cost. Do not repeat the rejected direct linear-buffer mapping bypass.
