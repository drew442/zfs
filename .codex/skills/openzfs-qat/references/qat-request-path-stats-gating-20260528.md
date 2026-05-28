# QAT Request-Path Stats Gating - 2026-05-28

## Purpose

Continue request-overhead reduction by gating diagnostic counters that describe
how a QAT compression request was constructed.

The previous shape-stat passes removed detailed buffer-shape accounting from
the normal path. The remaining always-on request-path counters still performed
atomic stat updates for buffer-slot reuse, page-array stack/heap/slot choices,
and request-slot reuse. These are useful for attribution but are not required
for correctness, fallback, watchdog, or progress tracking.

## Change

`zfs_qat_dc_shape_stats=profile` now disables these compression request-path
diagnostic counters:

- `dc_buffer_reuse_hits`
- `dc_buffer_reuse_misses`
- `dc_compress_page_array_stack_src`
- `dc_compress_page_array_heap_src`
- `dc_compress_page_array_slot_src`
- `dc_compress_page_array_stack_dst`
- `dc_compress_page_array_heap_dst`
- `dc_compress_page_array_slot_dst`
- `dc_compress_page_array_stack_scratch`
- `dc_compress_page_array_heap_scratch`
- `dc_compress_page_array_slot_scratch`
- `dc_compress_req_slot`

Set `zfs_qat_dc_shape_stats=1` when a benchmark needs request-path attribution.
With profile/default, derived benchmark fields for page-array path and
request-slot reuse are expected to be zero.

Correctness, QAT admission, fallback policy, async-cap behavior, wait
accounting, timing accounting, inflight accounting, and watchdog behavior are
unchanged.

## Validation

Host:

- `pve.drewnet.online`
- Kernel: `7.0.0-3-pve`
- Loaded and installed ZFS `srcversion`: `AD7B8686FC9E2032979E83E`
- QAT DC instances visible to ZFS: `12`
- Post-test profile restored to `zfs_qat_dc_profile=balanced` and
  `zfs_qat_dc_profile_recordsize=131072`
- `zpool status -x`: all pools healthy
- `dc_fails`: `0`
- `dc_watchdog_health`: `1`

Artifacts:

- `artifacts/request-path-gate-20260528/default-hdd-shape-profile.csv`
- `artifacts/request-path-gate-20260528/default-hdd-shape-1.csv`
- `artifacts/request-path-gate-20260528/default-nvme-shape-profile.csv`
- `artifacts/request-path-gate-20260528/default-nvme-shape-1.csv`
- `artifacts/request-path-gate-20260528/throughput1m-hdd-shape-profile.csv`
- `artifacts/request-path-gate-20260528/throughput1m-hdd-shape-1.csv`
- `artifacts/request-path-gate-20260528/throughput1m-nvme-shape-profile.csv`
- `artifacts/request-path-gate-20260528/throughput1m-nvme-shape-1.csv`
- `artifacts/request-path-gate-20260528/request-path-gate-summary.csv`

CSV validation:

- Eight raw benchmark CSVs were captured.
- Each raw CSV has four measurement rows and two summary rows.
- All CSV rows have `263` columns.
- All raw rows have `sha_ok=yes`.
- All raw rows have `dc_fails_delta=0`.

Diagnostic counter check:

| config | media | record | shape profile diag sum | shape=1 diag sum |
| --- | --- | ---: | ---: | ---: |
| default | hdd | 128K | 0 | 58400 |
| default | hdd | 1M | 0 | 0 |
| default | nvme | 128K | 0 | 58400 |
| default | nvme | 1M | 0 | 0 |
| throughput1m | hdd | 512K | 0 | 6830 |
| throughput1m | hdd | 1M | 0 | 5520 |
| throughput1m | nvme | 512K | 0 | 6520 |
| throughput1m | nvme | 1M | 0 | 5610 |

The `default`/`1M` rows are expected to stay at zero because the default
profile still caps QAT compression at `128K`, so those writes fall back to
software compression.

Performance context:

| config | media | record | profile ms | profile MiB/s | shape=1 ms | shape=1 MiB/s | QAT byte share profile/on |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| default | hdd | 128K | 1145.1 | 637.6 | 1071.9 | 681.1 | 100.0/100.0 |
| default | hdd | 1M | 979.6 | 745.6 | 942.8 | 774.5 | 0.0/0.0 |
| default | nvme | 128K | 826.5 | 883.4 | 841.5 | 867.4 | 100.0/100.0 |
| default | nvme | 1M | 708.7 | 1030.0 | 770.6 | 947.4 | 0.0/0.0 |
| throughput1m | hdd | 512K | 955.6 | 765.0 | 905.6 | 807.0 | 56.6/46.8 |
| throughput1m | hdd | 1M | 895.9 | 816.9 | 853.5 | 855.2 | 83.1/75.6 |
| throughput1m | nvme | 512K | 693.8 | 1052.0 | 694.7 | 1050.8 | 46.1/44.7 |
| throughput1m | nvme | 1M | 740.8 | 985.5 | 736.9 | 990.7 | 78.3/76.9 |

## Interpretation

This change removes normal-path diagnostic atomics but intentionally leaves
service counters and safety counters active. It should be treated as
observability gating, not as a policy change.

The measured elapsed-time rows are mixed and should not be treated as a clean
performance proof. Some paired rows also changed QAT byte share, so the retained
justification is structural overhead reduction: profile/default mode now avoids
these request-path diagnostic atomic updates, while focused benchmarks can turn
them back on with `zfs_qat_dc_shape_stats=1`.
