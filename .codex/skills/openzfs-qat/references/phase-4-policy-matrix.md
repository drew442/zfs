# Phase 4 QAT Policy Matrix

Date: 2026-05-17.

This note converts the Phase 4 tuning evidence into policy decisions. It is
intended to prevent old one-off tunables from being folded into the default path
without enough evidence or without checking whether they are compatible with the
current async path.

## Policy Axes

Record size is the correct first policy axis for QAT 1.x gzip write
compression. It predicts:

- QAT setup cost relative to useful compression work.
- Source and destination buffer-list size.
- Async cap pressure and fallback rate.
- Whether software gzip is consistently faster than the hardware path.

Record size is not sufficient by itself. The policy also needs:

- Active QAT DC instance count.
- Whether the async path is enabled.
- Whether the selected tunable is per-request or session-global.
- Performance-vs-ratio bias.
- Latency-vs-throughput bias.

## Current Constraints

- `zfs_qat_dc_async_cap_policy` is a per-request decision and can safely use
  record size and active DC instance count.
- `zfs_qat_dc_coalesce_src` and `zfs_qat_dc_coalesce_dst` can now run with the
  async path. They remain record-size/profile candidates because the measured
  results are mixed.
- `zfs_qat_cpa_dc_level` and `zfs_qat_cpa_dc_hufftype` are QAT DC session
  settings. They are global after QAT DC initialization and cannot currently
  vary per record without building multiple session sets per QAT instance.
- `zfs_qat_dc_max_instances`, the QAT DC/CY split, QAT checksum disablement,
  and QAT encryption disablement are hardware/resource allocation choices. They
  are not per-record tuning knobs.

## Decision Matrix

Legend:

```text
enable     Strong enough evidence for policy use now.
disable    Strong enough evidence to avoid for this record size.
manual     Useful operator knob, but not safe to automate yet.
blocked    Requires implementation work before it can participate.
unknown    Needs fresh benchmark evidence under the current DC6 async setup.
```

| Tunable | Scope | 8K | 16K | 32K | 64K | 128K | 256K | 1M | Policy Decision |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| Async QAT admission | per request | disable | disable | disable | disable | enable | enable | enable | Use record-size policy. |
| Async in-flight cap | per request | software | software | software | software | 768 on DC6 | 192 on DC6 | 96 on DC6 | Keep record-size/DC-count policy. |
| Source coalescing | per request path | manual | manual | manual | manual | disable | manual | manual | Technically unblocked, but repeat data does not justify policy enablement. |
| Destination coalescing | per request path | manual | manual | manual | manual | disable | manual | manual | Technically unblocked, but repeat data does not justify policy enablement. |
| Compression level | QAT session | manual | manual | manual | manual | manual | manual | manual | Bias-profile candidate, not per-record today. |
| Huffman type | QAT session | manual | manual | manual | manual | manual | manual | manual | Bias-profile candidate, not per-record today. |
| DC max instances | QAT init/global | manual | manual | manual | manual | manual | manual | manual | Keep as hardware allocation control. |
| QAT DC/CY split | QAT service config | manual | manual | manual | manual | manual | manual | manual | Keep as host-level deployment control. |

## Latest 1M Repeat

Source CSVs:

```text
/root/zfs-qat-phase4-async-dc6-policy-fixed-1m-repeat-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-policy-recordsize-1m-repeat-jobs4-20260517.csv

Repo copies:
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-dc6-policy-fixed-1m-repeat-jobs4-20260517.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-dc6-policy-recordsize-1m-repeat-jobs4-20260517.csv
```

Test settings:

```text
NumberCyInstances = 0
NumberDcInstances = 6
zfs_qat_dc_async=1
zfs_qat_dc_async_submit_retries=8
zfs_qat_dc_async_retry_us=100
zfs_qat_dc_async_max_inflight=96
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
zfs_qat_decompress_disable=1
VERIFY_MODE=sw
JOBS=4
ITERS=6
RECORDS=1M
```

Results:

```text
policy     mode avg_ms  MiB_s qat_share cap_skips ratio
fixed      qat  933.789 781.7 33.3%     2928      25.39x
fixed      sw   927.323 787.1 n/a       n/a       25.26x
recordsize qat 907.026 804.7 33.4%     2926      25.35x
recordsize sw  921.995 791.7 n/a       n/a       25.26x
```

Result: fixed and recordsize both use cap `96` for `1M`, and the measured
difference between them is noise-level. The recordsize policy should keep
`1M+ = 96` on DC6.

## Async Coalescing Follow-Up

Source CSVs:

```text
/root/zfs-qat-phase4-async-src-coalesce-smoke-20260517.csv
/root/zfs-qat-phase4-async-dst-coalesce-smoke-20260517.csv
/root/zfs-qat-phase4-async-coalesce-off-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-src-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-dst-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-both-jobs4-20260517.csv

Repo copies:
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-src-coalesce-smoke-20260517.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-dst-coalesce-smoke-20260517.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-coalesce-off-jobs4-20260517.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-coalesce-src-jobs4-20260517.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-coalesce-dst-jobs4-20260517.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-coalesce-both-jobs4-20260517.csv
```

Smoke results:

- Async plus source coalescing passed SHA verification and reduced source
  buffers to `1`.
- Async plus destination coalescing passed SHA verification and reduced
  destination plus scratch output buffers to `1`.

Three-iteration jobs=4 results:

```text
case record qat_ms   sw_ms    qat_vs_sw qat_share src_bufs dst_total_bufs
off  128K   1112.168 1091.258 +1.9%     40.1%     32.0     37.0
off  256K   1013.292 1008.034 +0.5%     30.8%     64.0     73.0
off  1M     986.959  914.903  +7.9%     33.3%     256.0    289.0
src  128K   1096.154 1058.649 +3.5%     36.6%     1.0      37.0
src  256K   975.573  964.011  +1.2%     27.1%     1.0      73.0
src  1M     876.813  928.709  -5.6%     32.7%     1.0      289.0
dst  128K   1161.276 1100.953 +5.5%     39.7%     32.0     1.0
dst  256K   979.081  1006.882 -2.8%     28.5%     64.0     1.0
dst  1M     933.987  1038.753 -10.1%    35.5%     256.0    1.0
both 128K   1201.735 1040.997 +15.4%    43.5%     1.0      1.0
both 256K   960.724  999.667  -3.9%     26.9%     1.0      1.0
both 1M     932.046  908.515  +2.6%     35.3%     1.0      1.0
```

Result: coalescing should not be globally enabled. It is technically unblocked
for async and useful as a profile input, but the policy should avoid it at
`128K`. In this pass, `256K` favored coalescing, especially both source and
destination together, while `1M` favored source-only among the QAT rows.

## Focused Coalescing Repeat

Source CSVs:

```text
/root/zfs-qat-phase4-async-coalesce-off-focus-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-src-focus-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-dst-focus-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-both-focus-jobs4-20260517.csv

Repo copies:
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-coalesce-off-focus-jobs4-20260517.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-coalesce-src-focus-jobs4-20260517.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-coalesce-dst-focus-jobs4-20260517.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-coalesce-both-focus-jobs4-20260517.csv
```

Focused repeat settings:

```text
NumberCyInstances = 0
NumberDcInstances = 6
zfs_qat_dc_async=1
zfs_qat_dc_async_submit_retries=8
zfs_qat_dc_async_retry_us=100
zfs_qat_dc_async_max_inflight=96
zfs_qat_dc_async_cap_policy=recordsize
zfs_qat_decompress_disable=1
VERIFY_MODE=sw
JOBS=4
ITERS=6
RECORDS="256K 1M"
```

Results:

```text
case record qat_ms   sw_ms    qat_vs_sw qat_share src_bufs dst_total_bufs
off  256K   954.435  1064.672 -10.4%    27.0%     64.0     73.0
off  1M     924.087  1019.278 -9.3%     33.5%     256.0    289.0
src  256K   1000.454 1003.532 -0.3%     27.5%     1.0      73.0
src  1M     965.245  1083.916 -10.9%    33.7%     1.0      289.0
dst  256K   971.320  982.247  -1.1%     27.1%     64.0     1.0
dst  1M     960.360  932.231  +3.0%     33.5%     256.0    1.0
both 256K   986.401  987.878  -0.1%     27.7%     1.0      1.0
both 1M     911.303  909.951  +0.1%     35.4%     1.0      1.0
```

Result: the focused repeat does not justify automatic coalescing. The best
`256K` QAT row was coalescing off. The best `1M` QAT row was both coalescing,
but it was effectively equal to software and only modestly faster than off in a
noisy window. Keep source and destination coalescing as manual experimental
knobs until a different workload or repeated matrix shows a stable win.

## Bias Profiles

Bias profiles are useful, but they should be layered on top of the record-size
admission/cap policy rather than replacing it.

Recommended initial profile model:

```text
zfs_qat_dc_policy=fixed|recordsize|profile
zfs_qat_dc_perf_bias=balanced|latency|throughput
zfs_qat_dc_ratio_bias=balanced|performance|ratio
```

Initial behavior should be conservative:

- `balanced`: current recordsize/DC-count admission and cap policy.
- `latency`: same as balanced, but avoid any record size that does not
  repeatedly beat software.
- `throughput`: allow higher measured caps where repeated throughput wins exist,
  never uncapped.
- `performance`: eventually prefer lower QAT compression level or static
  Huffman if repeated evidence proves a speed win and the operator accepts ratio
  loss.
- `ratio`: prefer dynamic Huffman and higher QAT compression level, accepting
  higher latency only when explicitly selected.

## Implementation Order

1. Keep the current `recordsize` async cap policy as the default candidate for
   QAT 1.x async gzip.
2. Keep coalescing out of automatic policy for now. It is technically
   compatible with async, but the focused repeat does not show a stable win.
3. If coalescing is revisited, test a second data source or a workload with a
   materially different compression ratio before adding profile behavior.
4. Do not make compression level or Huffman type per-record until the code can
   maintain multiple QAT DC sessions per instance.
5. Add bias-profile parameters only after the policy actions they control are
   implementable and benchmark-backed.
