# QAT Async Page Arrays And Scratch Reuse - 2026-05-23

Purpose: reduce ZFS-side per-request allocation overhead in the QAT DC wrapper
without changing QAT 1.x hardware assumptions or relying on a fixed card count.

## Change

- Async compression requests now embed small page-pointer arrays sized to the
  existing `QAT_DC_STACK_MAX_PAGES` threshold. This avoids separate `kmem_zalloc`
  page-array allocations for async requests at 128K and smaller records.
- Existing per-instance QAT buffer reuse slots now retain a scratch buffer.
  Requests that acquire a reusable slot use that scratch buffer instead of
  allocating and freeing the scratch area for every compressed record.
- Requests that cannot acquire a reusable slot still fall back to per-request
  scratch allocation, preserving existing behavior and software fallback.

This is a general request-path optimization. It is not tied to one-card or
two-card topology.

## Validation Host State

Host: `pve.drewnet.online`

Kernel: `7.0.0-3-pve`

Loaded ZFS module after the change:

```text
srcversion: FC2F209AA09A507B8C7F723
depends: spl,qat_api
dc_instances=12
dc_fails=0
```

Benchmark configuration:

```text
records: 128K, 1M
jobs: 4, 8
iterations: 5
modes: qat, sw
verify: sw
zfs_qat_dc_profile=throughput
zfs_qat_dc_profile_recordsize=1048576
completion mode: interrupt
```

The host was restored afterward to the default 128K balanced profile:

```text
options zfs zfs_qat_compress_disable=0 zfs_qat_checksum_disable=1 zfs_qat_encrypt_disable=1 zfs_qat_cpa_dc_level=profile zfs_qat_dc_max_buf_size=profile
zfs_qat_dc_profile_recordsize=131072
zfs_qat_dc_profile=balanced
dc_instances=12
dc_fails=0
```

## Artifacts

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-page-stack-records128k1m-jobs4-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-page-stack-records128k1m-jobs8-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-scratch-reuse-records128k1m-jobs4-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-scratch-reuse-records128k1m-jobs8-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-scratch-reuse-warm-records128k1m-jobs4-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-scratch-reuse-warm-records128k1m-jobs8-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-scratch-reuse-summary-20260523.csv
```

Build and reboot logs were imported under:

```text
.codex/skills/openzfs-qat/artifacts/host-root-import-20260523/
```

## Results

Positive `QAT vs SW` means QAT mode was faster than same-window software gzip.

| Variant | Record | Jobs | QAT ms | SW ms | QAT vs SW | QAT share | QAT CPU s/GiB | Scratch alloc ns/request |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| async page arrays | 128K | 4 | 1146.3 | 1096.1 | 4.6% slower | 70.2% | 9.09 | 2772 |
| async page arrays | 128K | 8 | 1670.6 | 1469.4 | 13.7% slower | 74.3% | 9.05 | 902 |
| async page arrays | 1M | 4 | 873.4 | 999.1 | 12.6% faster | 76.5% | 5.49 | 79011 |
| async page arrays | 1M | 8 | 1281.3 | 1440.2 | 11.0% faster | 78.0% | 6.01 | 1427 |
| scratch reuse cold | 128K | 4 | 1223.2 | 1163.8 | 5.1% slower | 95.8% | 9.81 | 1819 |
| scratch reuse cold | 128K | 8 | 1586.3 | 1665.1 | 4.7% faster | 75.4% | 8.89 | 393 |
| scratch reuse cold | 1M | 4 | 889.8 | 948.0 | 6.1% faster | 83.8% | 5.60 | 101170 |
| scratch reuse cold | 1M | 8 | 1434.3 | 1591.0 | 9.9% faster | 82.2% | 6.45 | 210 |
| scratch reuse warm | 128K | 4 | 1157.0 | 1049.7 | 10.2% slower | 84.4% | 9.38 | 621 |
| scratch reuse warm | 128K | 8 | 1670.7 | 1511.4 | 10.5% slower | 77.1% | 9.62 | 475 |
| scratch reuse warm | 1M | 4 | 878.6 | 934.9 | 6.0% faster | 77.4% | 5.64 | 232 |
| scratch reuse warm | 1M | 8 | 1238.7 | 1469.2 | 15.7% faster | 75.3% | 6.13 | 205 |

## Interpretation

- Embedded async page arrays are safe and mechanically useful: the loaded module
  completed QAT tests with `dc_fails=0`, and 128K async requests now use the
  stack-page accounting path instead of the heap-page accounting path.
- Scratch reuse substantially reduces steady-state scratch allocation cost once
  reusable slots are warm. The warm 1M rows reached about `205-232 ns/request`
  of scratch allocation accounting, compared with `79 us/request` in the
  page-stack 1M/jobs=4 baseline.
- Elapsed-time impact is mixed. The best result was the warm 1M/jobs=8 row:
  `1238.7 ms`, which is `3.3%` faster than the page-stack-only row and `15.7%`
  faster than same-window software gzip.
- 128K remains noisy and is not improved enough to claim this as a 128K latency
  fix. The 128K rows still require separate policy or request-shape work.

## Decision

Keep the code change for now because it removes avoidable per-request allocation
work and improves the larger-record concurrent row, while preserving fallback.

Do not promote a new profile default based only on this result. The next
benchmark should retest this change in the full profile matrix (`64K`, `128K`,
`256K`, `512K`, `1M`; jobs `1`, `4`, `8`) and classify rows where elapsed time
improves mainly due to QAT share changes rather than faster QAT service.
