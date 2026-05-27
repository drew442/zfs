# QAT Aggregate Shape-Stats Gating - 2026-05-27

## Purpose

Continue request-overhead reduction by removing the remaining default-path
request-shape diagnostic accounting.

The earlier `zfs_qat_dc_shape_stats` pass disabled detailed per-flat-buffer
alignment accounting but intentionally left aggregate source, destination,
scratch, and max-buffer counters always enabled. Those counters are useful for
benchmark attribution, but they still cost several atomic/stat updates for
every QAT compression request.

## Change

`zfs_qat_dc_shape_stats=profile` now disables all compression request-shape
diagnostics, including aggregate counters:

- `dc_compress_src_buffers`
- `dc_compress_dst_buffers`
- `dc_compress_add_buffers`
- `dc_compress_dst_total_buffers`
- `dc_compress_src_buffers_max`
- `dc_compress_dst_buffers_max`
- `dc_compress_add_buffers_max`
- `dc_compress_dst_total_buffers_max`

Set `zfs_qat_dc_shape_stats=1` when a benchmark or investigation needs these
fields. With profile/default, derived benchmark fields such as
`qat_src_buffers_per_req` and `qat_dst_total_buffers_per_req` are expected to be
zero or `na`.

Correctness, QAT admission, fallback policy, wait accounting, and timing
accounting are unchanged.

## Validation

Host: `pve.drewnet.online`

Kernel: `7.0.0-3-pve`

QAT hardware: two DH895XCC cards, `12` DC instances

ZFS module `srcversion`: `C98C6329FB3355BE43ADAE2`

Artifacts:

- `artifacts/aggregate-shape-gate-20260527/default-hdd-shape-profile.csv`
- `artifacts/aggregate-shape-gate-20260527/default-hdd-shape-1.csv`
- `artifacts/aggregate-shape-gate-20260527/default-nvme-shape-profile.csv`
- `artifacts/aggregate-shape-gate-20260527/default-nvme-shape-1.csv`
- `artifacts/aggregate-shape-gate-20260527/throughput1m-hdd-shape-profile.csv`
- `artifacts/aggregate-shape-gate-20260527/throughput1m-hdd-shape-1.csv`
- `artifacts/aggregate-shape-gate-20260527/throughput1m-nvme-shape-profile.csv`
- `artifacts/aggregate-shape-gate-20260527/throughput1m-nvme-shape-1.csv`
- `artifacts/aggregate-shape-gate-20260527/summary.csv`

All measurement rows:

- `sha_ok=yes`
- `dc_fails_delta=0`
- raw CSV files have consistent `263`-column width

The host was restored after testing:

- `zfs_qat_dc_profile=balanced`
- `zfs_qat_dc_profile_recordsize=131072`
- `zfs_qat_dc_shape_stats=profile`
- `zfs_qat_dc_timing_stats=profile`
- pools online
- `dc_fails=0`
- `dc_watchdog_health=1`

## Result Summary

Default balanced profile, `128K` sync compression:

| media | shape stats | elapsed ms | MiB/s | CPU active % | QAT byte share % | src bufs/req | dst bufs/req |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| HDD | off (`profile`) | 1111.035 | 657.050 | 5.163 | 100.000 | 0 | 0 |
| HDD | on (`1`) | 1085.051 | 673.267 | 5.177 | 100.000 | 32 | 37 |
| NVMe | off (`profile`) | 826.329 | 883.397 | 5.257 | 100.000 | 0 | 0 |
| NVMe | on (`1`) | 835.318 | 873.870 | 5.117 | 100.000 | 32 | 37 |

Throughput/1M profile, accelerated larger-record coverage:

| media | record | shape stats | elapsed ms | MiB/s | CPU active % | QAT byte share % | src bufs/req | dst bufs/req |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| HDD | 512K | off (`profile`) | 903.959 | 807.710 | 9.943 | 55.620 | 0 | 0 |
| HDD | 512K | on (`1`) | 981.146 | 745.593 | 10.360 | 50.783 | 128 | 145 |
| HDD | 1M | off (`profile`) | 1008.496 | 725.650 | 5.387 | 97.497 | 0 | 0 |
| HDD | 1M | on (`1`) | 906.181 | 805.947 | 7.050 | 79.370 | 256 | 289 |
| NVMe | 512K | off (`profile`) | 728.224 | 1002.427 | 13.930 | 47.630 | 0 | 0 |
| NVMe | 512K | on (`1`) | 740.228 | 986.590 | 13.677 | 46.307 | 128 | 145 |
| NVMe | 1M | off (`profile`) | 770.675 | 948.147 | 8.463 | 79.687 | 0 | 0 |
| NVMe | 1M | on (`1`) | 883.015 | 826.770 | 6.240 | 96.310 | 256 | 289 |

## Interpretation

This is a structural request-overhead cleanup. It removes diagnostic atomics
from the normal sync and async compression paths while preserving opt-in
observability for request-shape analysis.

The structural behavior is correct: profile/default rows no longer populate the
aggregate shape deltas, while `zfs_qat_dc_shape_stats=1` restores expected
request-shape counts for accelerated records.

Elapsed-time results are mixed and should not be treated as pure shape-counter
cost. Some paired rows also changed QAT byte share, especially larger async
records, so the retained justification is lower normal-path diagnostic work, not
a claimed universal throughput improvement.

Benchmark consumers must explicitly set `zfs_qat_dc_shape_stats=1` when they
need `qat_src_buffers_per_req`, `qat_dst_total_buffers_per_req`, or related
shape fields for attribution.
