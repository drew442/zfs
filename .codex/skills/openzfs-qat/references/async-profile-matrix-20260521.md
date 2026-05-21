# Async Profile Matrix - 2026-05-21

Purpose: complete the async/profile validation pass after the async timeout
fallback work. This matrix compares software gzip, synchronous QAT gzip, and
async QAT gzip across record sizes, job counts, and QAT delivery modes.

## Test Shape

- Host: `pve.drewnet.online`
- Cards: 2x DH895XCC, DC-only QAT driver configuration
- Records: `64K`, `128K`, `256K`, `512K`, `1M`
- Jobs: `1`, `4`, `8`
- Iterations: `3`
- Delivery modes: interrupt and ZFS DC polling
- Temporary boot setting: `zfs_qat_dc_profile_recordsize=1048576`
- Temporary readback setting: `zfs_qat_decompress_disable=1`
- Host was restored afterward to `zfs_qat_dc_profile_recordsize=131072`,
  `zfs_qat_dc_poll=profile`, and `[KERNEL_QAT] Dc*IsPolled = 0`.

Raw CSV artifacts:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-matrix-interrupt-async-on-1jobs-20260521.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-matrix-interrupt-async-off-1jobs-20260521.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-matrix-interrupt-async-on-4jobs-20260521.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-matrix-interrupt-async-off-4jobs-20260521.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-matrix-interrupt-async-on-8jobs-20260521.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-matrix-interrupt-async-off-8jobs-20260521.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-matrix-poll-async-on-1jobs-20260521.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-matrix-poll-async-off-1jobs-20260521.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-matrix-poll-async-on-4jobs-20260521.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-matrix-poll-async-off-4jobs-20260521.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-matrix-poll-async-on-8jobs-20260521.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-matrix-poll-async-off-8jobs-20260521.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-async-matrix-summary-20260521.csv
```

## Interpretation Rules

- `async_qat` rows are not necessarily pure hardware results. Nonzero cap skip
  percentage means the policy intentionally fell back to software for some
  writes.
- `64K` async rows are software-fallback rows by current policy because the
  record-size cap does not admit QAT below `128K`.
- Lower elapsed time with lower QAT byte share is a hybrid-policy win, not proof
  that QAT engine service time improved.
- Compression ratio did not materially change between software, sync QAT, and
  async QAT in this matrix. Ratio mainly followed record size, from about
  `11.55x` at `64K` to about `25.1x` at `1M`.
- Watchdog, quarantine, and poll failure counters remained clean in the matrix:
  no request timeouts, no runtime disables, no retained quarantine buffers, and
  no poll failures were observed.

## Summary

- Async QAT is useful for concurrent larger-record throughput. The strongest
  interrupt wins were `JOBS=8`, `512K` at `18.4%` faster than software and
  `JOBS=8`, `1M` at `25.7%` faster than software.
- Async QAT is not a safe default for latency or balanced operation. It regressed
  several `128K` and `256K` rows, especially in polling mode.
- Polling mode is not a profile default. It produced useful large-record wins,
  but it did not consistently beat interrupt mode and still needs explicit host
  configuration in the QAT driver.
- The current implementation already matches the evidence: `balanced` and
  `latency` keep async disabled; `throughput` and `offload` enable async; the
  `profile` cap policy remains record-size and DC-count aware.

## Comparison Table

Negative `Async vs SW` or `Async vs Sync` means async was faster. CPU is active
CPU seconds per GiB copied.

| Delivery | Jobs | Record | SW ms | Sync ms | Async ms | Async vs SW | Async vs Sync | QAT byte % | Cap skip % | Async CPU s/GiB | SW CPU s/GiB | Outcome |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| interrupt | 1 | 64K | 874.3 | 881.2 | 934.0 | +6.8% | +6.0% | 0.0 | 100.0 | 12.37 | 12.39 | software fallback by cap |
| interrupt | 1 | 128K | 760.3 | 829.9 | 748.2 | -1.6% | -9.8% | 100.0 | 0.0 | 5.90 | 11.04 | small async win |
| interrupt | 1 | 256K | 661.1 | 654.2 | 697.0 | +5.4% | +6.5% | 78.7 | 21.3 | 5.92 | 10.34 | CPU-offload only |
| interrupt | 1 | 512K | 598.4 | 585.5 | 605.2 | +1.1% | +3.4% | 74.3 | 25.7 | 5.37 | 9.77 | CPU-offload only |
| interrupt | 1 | 1M | 584.9 | 584.8 | 586.2 | +0.2% | +0.2% | 83.5 | 16.8 | 4.78 | 10.08 | CPU-offload only |
| interrupt | 4 | 64K | 1251.2 | 1233.6 | 1262.0 | +0.9% | +2.3% | 0.0 | 100.0 | 13.78 | 13.59 | software fallback by cap |
| interrupt | 4 | 128K | 1079.0 | 1101.1 | 1127.3 | +4.5% | +2.4% | 69.4 | 30.6 | 8.91 | 12.31 | CPU-offload only |
| interrupt | 4 | 256K | 986.4 | 1064.3 | 1021.9 | +3.6% | -4.0% | 47.2 | 52.8 | 8.16 | 11.78 | CPU-offload only |
| interrupt | 4 | 512K | 988.6 | 1087.3 | 902.8 | -8.7% | -17.0% | 49.5 | 50.5 | 7.39 | 11.45 | async throughput win |
| interrupt | 4 | 1M | 935.2 | 1029.3 | 839.0 | -10.3% | -18.5% | 50.4 | 49.8 | 7.05 | 11.34 | async throughput win |
| interrupt | 8 | 64K | 1767.6 | 1852.5 | 1749.1 | -1.1% | -5.6% | 0.0 | 100.0 | 13.74 | 13.89 | software fallback by cap |
| interrupt | 8 | 128K | 1569.1 | 1551.1 | 1531.9 | -2.4% | -1.2% | 74.2 | 25.8 | 8.23 | 12.51 | small async win |
| interrupt | 8 | 256K | 1345.9 | 1460.2 | 1391.1 | +3.4% | -4.7% | 59.2 | 40.9 | 7.64 | 12.14 | CPU-offload only |
| interrupt | 8 | 512K | 1534.3 | 1483.9 | 1252.0 | -18.4% | -15.6% | 51.9 | 48.2 | 7.85 | 12.72 | async throughput win |
| interrupt | 8 | 1M | 1734.7 | 1462.7 | 1289.0 | -25.7% | -11.9% | 65.1 | 35.4 | 7.02 | 15.68 | async throughput win |
| poll | 1 | 64K | 942.9 | 886.1 | 939.9 | -0.3% | +6.1% | 0.0 | 100.0 | 12.13 | 12.33 | software fallback by cap |
| poll | 1 | 128K | 737.5 | 829.0 | 778.2 | +5.5% | -6.1% | 99.1 | 0.9 | 5.70 | 11.10 | CPU-offload only |
| poll | 1 | 256K | 675.2 | 678.7 | 665.6 | -1.4% | -1.9% | 76.8 | 23.2 | 5.56 | 10.25 | small async win |
| poll | 1 | 512K | 591.7 | 594.3 | 573.2 | -3.1% | -3.6% | 74.6 | 25.4 | 5.54 | 9.20 | small async win |
| poll | 1 | 1M | 581.8 | 580.4 | 593.9 | +2.1% | +2.3% | 89.1 | 11.1 | 4.59 | 9.41 | CPU-offload only |
| poll | 4 | 64K | 1276.4 | 1275.8 | 1268.4 | -0.6% | -0.6% | 0.0 | 100.0 | 14.50 | 13.69 | software fallback by cap |
| poll | 4 | 128K | 1079.3 | 1086.8 | 1190.0 | +10.3% | +9.5% | 67.4 | 32.6 | 8.78 | 12.24 | CPU-offload only |
| poll | 4 | 256K | 960.9 | 1014.8 | 1037.7 | +8.0% | +2.3% | 46.9 | 53.1 | 8.23 | 11.83 | CPU-offload only |
| poll | 4 | 512K | 981.4 | 1074.0 | 941.5 | -4.1% | -12.3% | 47.9 | 52.1 | 7.58 | 11.02 | small async win |
| poll | 4 | 1M | 991.9 | 1081.5 | 862.2 | -13.1% | -20.3% | 55.6 | 44.5 | 7.07 | 11.48 | async throughput win |
| poll | 8 | 64K | 1805.9 | 1786.7 | 1751.4 | -3.0% | -2.0% | 0.0 | 100.0 | 14.25 | 13.81 | software fallback by cap |
| poll | 8 | 128K | 1481.0 | 1564.8 | 1661.2 | +12.2% | +6.2% | 68.4 | 31.6 | 8.72 | 12.59 | CPU-offload only |
| poll | 8 | 256K | 1356.4 | 1502.9 | 1394.0 | +2.8% | -7.2% | 57.2 | 42.8 | 7.77 | 12.14 | CPU-offload only |
| poll | 8 | 512K | 1416.1 | 1491.3 | 1215.5 | -14.2% | -18.5% | 49.6 | 50.4 | 8.27 | 12.04 | async throughput win |
| poll | 8 | 1M | 1470.3 | 1622.2 | 1281.1 | -12.9% | -21.0% | 59.0 | 41.5 | 7.39 | 12.25 | async throughput win |

## Profile Decision

No code change is required for profile defaults from this matrix.

- Keep `zfs_qat_dc_profile=balanced` as the default.
- Keep async disabled for `balanced` and `latency`.
- Keep async enabled only for `throughput` and `offload`.
- Keep `zfs_qat_dc_async_cap_policy=profile` as the default. The current
  implementation already uses record-size caps, scales against visible DC
  instances, and only uses the higher `1M+` throughput cap when the operator has
  selected `throughput` or `offload` with `zfs_qat_dc_profile_recordsize >= 1M`.
- Do not make polling a profile default. Keep it as an explicit operator option.

## Step 5 Plan: QAT Request-Overhead Target

Goal: reduce per-request cost and QAT service wait without hiding regressions
behind software fallback. The work should optimize QAT generally for one or more
QAT 1.x cards, not for a fixed two-card layout.

1. Establish a clean overhead baseline. Extend the benchmark summary to always
   report per-request setup, submit, wait, callback, cleanup, QAT byte share, cap
   skip share, CPU seconds per GiB, and compression ratio for `64K` through `1M`
   at `JOBS=1/4/8`.
2. Add request-shape counters. Track scatter/gather entry counts, source
   coalescing decisions, destination buffer count, scratch size, bound size,
   allocation path, and sync versus async completion path by record size.
3. Separate ZFS wrapper overhead from QAT service time. Use existing ZFS timing
   and QAT driver timing counters, then add only the missing timing buckets
   needed to explain request construction and completion callback cost.
4. Optimize the ZFS request path first where evidence points to CPU-side cost:
   reuse request contexts, reduce per-request allocation/free, avoid avoidable
   buffer-list rebuilds, and keep coalescing conditional on measured benefit.
5. Optimize admission policy second. Keep fallback available, but evaluate
   profile policy using both elapsed time and QAT-share-normalized metrics so
   profile changes do not merely win by sending more work to software.
6. Evaluate QAT-side settings after the request shape is visible: compression
   level, Huffman mode, instance count, ring/bank configuration, polling mode,
   and interrupt coalescing. Keep QAT 2.0+ features out of scope unless the same
   setting exists and applies to QAT 1.x.
7. Validate each candidate with the same matrix shape: `64K`, `128K`, `256K`,
   `512K`, `1M`; `JOBS=1/4/8`; software, sync QAT, and async QAT; interrupt
   mode first, polling only when the candidate specifically touches polling.
8. Promotion criteria: elapsed time must improve without a material compression
   ratio regression, CPU seconds per GiB must stay below software, correctness
   must pass, and any improvement with lower QAT byte share must be documented
   as a hybrid-policy win rather than a QAT engine improvement.

Immediate next target: add the request-shape counters and summary reporting,
then rerun a narrow baseline before changing allocation or coalescing behavior.
