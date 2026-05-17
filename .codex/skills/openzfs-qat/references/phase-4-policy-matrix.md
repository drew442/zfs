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

## Current Hard Constraints

- `zfs_qat_dc_async_cap_policy` is a per-request decision and can safely use
  record size and active DC instance count.
- `zfs_qat_dc_coalesce_src` and `zfs_qat_dc_coalesce_dst` currently disable the
  async path because `qat_dc_compress_async_enabled()` requires both coalescing
  knobs to be off.
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
| Source coalescing | module/per request path | blocked | blocked | blocked | blocked | unknown | unknown | disable | Do not fold into async policy until async supports coalesced buffers. |
| Destination coalescing | module/per request path | blocked | blocked | blocked | blocked | unknown | unknown | unknown | Do not fold into async policy until async supports coalesced buffers. |
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
2. Do not enable source or destination coalescing in the async policy until the
   async path can use coalesced buffers without disabling itself.
3. If coalescing remains interesting, implement async-compatible source and
   destination coalescing as separate patches, then repeat 128K, 256K, and 1M
   benchmarks under DC6 async.
4. Do not make compression level or Huffman type per-record until the code can
   maintain multiple QAT DC sessions per instance.
5. Add bias-profile parameters only after the policy actions they control are
   implementable and benchmark-backed.

