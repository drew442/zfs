# Phase 4 QAT Policy Matrix

Date: 2026-05-18.

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
  record size and active DC instance count. The balanced policy should not
  blindly scale caps linearly beyond measured DC6 ceilings; the first DC12
  linear test increased QAT share but regressed elapsed time.
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
| Async in-flight cap | per request | software | software | software | software | 128 per DC, DC6 ceiling | 32 per DC, DC6 ceiling | 16 per DC, DC6 ceiling | Keep conservative record-size/DC-count policy; test higher caps as profile behavior. |
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

## DC Instance Scaling

The policy target is OpenZFS/QAT generally, not the current card count on
`pve.drewnet.online`. Cap decisions should therefore use active ZFS QAT DC
instances, not card count. A host may expose one, two, or more cards, and the
useful scheduling unit for this code path is the initialized DC instance count.

The first DC12 experiment tested linear scaling from the DC6 caps:

```text
record DC6 cap linear DC12 cap
128K   768     1536
256K   192     384
1M     96      192
```

Result versus the prior DC6-ceiling dual-card run:

```text
jobs record elapsed_delta qat_byte_delta fallback_delta
4    128K   +4.3%         +35.3pp        -35.2pp
4    256K   +10.0%        +21.1pp        -21.1pp
4    1M     +8.1%         +28.3pp        -28.2pp
8    128K   +3.2%         +14.9pp        -14.9pp
8    256K   +5.0%         +13.9pp        -13.9pp
8    1M     +1.0%         +24.9pp        -24.7pp
```

Interpretation:

- Linear DC12 scaling did what it was supposed to do mechanically: QAT byte
  share rose and software fallback fell.
- The elapsed-time result got worse in every row compared with the conservative
  dual-card policy window.
- The balanced record-size policy should use active DC count, but cap at the
  measured DC6 ceiling until a profile-specific sweep proves a higher cap is
  useful for a concrete bias such as CPU offload or throughput.
- Higher caps remain valid candidates for a future `throughput` or `offload`
  profile. They should not be silently promoted to the default balanced policy.

### Midpoint Profile Sweep

The next sweep tested caps between the balanced DC6 ceiling and linear DC12
endpoint at `JOBS=8`.

```text
record cap  kind      qat_ms   sw_ms    qat_vs_sw qat_byte fallback
128K   768  balanced  1571.288 1483.652 +5.9%     67.2%    32.8%
128K   1024 midpoint  1702.346 1749.028 -2.7%     76.9%    23.1%
128K   1280 midpoint  1581.038 1558.804 +1.4%     72.6%    27.4%
128K   1536 linear    1622.120 1522.491 +6.5%     82.1%    18.0%
256K   192  balanced  1383.344 1444.475 -4.2%     49.0%    51.0%
256K   256  midpoint  1481.648 1453.743 +1.9%     62.3%    37.8%
256K   320  midpoint  1475.804 1402.650 +5.2%     60.7%    39.4%
256K   384  linear    1452.347 1503.671 -3.4%     62.9%    37.1%
1M     96   balanced  1346.845 1455.752 -7.5%     48.2%    52.1%
1M     128  midpoint  1390.585 1452.624 -4.3%     57.2%    43.3%
1M     160  midpoint  1317.327 1400.008 -5.9%     63.3%    37.2%
1M     192  linear    1360.608 1561.182 -12.8%    73.1%    27.5%
```

Policy interpretation:

- Keep `128K` and `256K` on the balanced caps. Higher caps increase QAT share
  but do not produce a stable elapsed-time win.
- Repeat `1M` cap `160`; it is the only new cap that improved elapsed time while
  increasing QAT byte share and lowering system CPU cost versus the balanced
  cap in this sweep.
- Treat `1M` cap `192` as an offload-biased candidate only. It gives more QAT
  share and lower system CPU, but it is slower than cap `160` in this pass.

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
3. Repeat the `1M` cap `160` candidate before adding a throughput/offload
   profile action.
4. If coalescing is revisited, test a second data source or a workload with a
   materially different compression ratio before adding profile behavior.
5. Do not make compression level or Huffman type per-record until the code can
   maintain multiple QAT DC sessions per instance.
6. Add bias-profile parameters only after the policy actions they control are
   implementable and benchmark-backed.
