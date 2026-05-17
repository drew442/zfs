# Phase 4 Async Queue Spike

Run date: 2026-05-16.

## Finding

The next useful QAT gzip performance target is a callback-driven ZIO integration,
not another small allocation or buffer-list tuning pass.

The current implementation already submits QAT requests asynchronously at the
QAT API layer, but immediately blocks the calling ZIO worker with
`wait_for_completion()`. Moving the same synchronous helper to a generic taskq
would only move the blocked wait to a different worker and add dispatch
overhead. It would not materially change the hardware work, the QAT service
time, or the per-block dependency chain.

## Evidence

Current synchronous path:

```text
module/zfs/zio.c
  zio_write_compress()
    zio_compress_data()

module/zfs/zio_compress.c
  zio_compress_data()
    ci->ci_compress()

module/zfs/gzip.c
  zfs_gzip_compress_buf()
    qat_compress(QAT_COMPRESS, ...)

module/os/linux/zfs/qat_compress.c
  qat_compress()
    qat_compress_impl()
      cpaDcCompressData(..., &complete)
      wait_for_completion(&complete)
```

The QAT callback currently only completes a Linux completion object:

```text
qat_dc_callback(void *p_callback, CpaStatus status)
```

The ZIO pipeline can suspend a stage by returning `NULL`. On later execution,
`__zio_execute()` resumes from the current `io_stage` and advances to the next
enabled stage. This is already used by stages such as `ZIO_STAGE_ISSUE_ASYNC`
and by parent/child wait points.

The write compression stage is before encryption, checksum generation, DVA
allocation, and physical I/O:

```text
ZIO_STAGE_WRITE_COMPRESS
ZIO_STAGE_ENCRYPT
ZIO_STAGE_CHECKSUM_GENERATE
ZIO_STAGE_DVA_THROTTLE
ZIO_STAGE_DVA_ALLOCATE
```

Therefore, any async compression completion must finalize the compressed data
before the pipeline advances past `ZIO_STAGE_WRITE_COMPRESS`.

## Rejected Option: Taskq-Wrapped Synchronous QAT

Rejected shape:

```text
zio_write_compress()
  dispatch synchronous qat_compress() to taskq
  wait for taskq result
```

Why this is not enough:

- It still blocks a kernel worker for each block.
- It keeps the same per-block `wait_for_completion()` behavior.
- It adds taskq dispatch and synchronization overhead.
- It does not improve software fallback semantics.
- It does not create useful ZIO-level overlap after the compression stage.

This option may reduce time spent specifically in the original ZIO issue worker,
but it does not address the measured throughput/latency gap.

## Viable Option: Callback-Driven ZIO Suspend/Resume

The viable design is to split QAT compression into submit and finish phases, then
teach `ZIO_STAGE_WRITE_COMPRESS` to suspend while QAT owns the in-flight request.

First execution of `ZIO_STAGE_WRITE_COMPRESS`:

1. Detect an eligible gzip/QAT write.
2. Borrow or allocate the source and destination buffers and keep them owned by
   an async request object.
3. Build QAT source/destination buffer lists.
4. Submit `cpaDcCompressData()` with a request object callback.
5. Set QAT async state on the `zio_t`.
6. Rewind `zio->io_stage` to the previous pipeline stage.
7. Return `NULL` to suspend this ZIO without blocking.

QAT callback:

1. Record QAT API status and result metadata in the request object.
2. Mark the request complete.
3. Dispatch the ZIO back to a ZIO taskq, preferably through the normal
   `zio_interrupt()` path or an equivalent safe resume path.
4. Do not run heavyweight finalization directly in the QAT callback unless QAT
   driver callback context is proven safe for that work.

Second execution of `ZIO_STAGE_WRITE_COMPRESS`:

1. See that the QAT request is complete.
2. Finalize header/footer and checksum handling.
3. Apply incompressible and overflow policy.
4. On success, push the compressed ABD transform and advance the write pipeline.
5. On QAT failure, run software gzip while the source data is still available.
6. Return borrowed ABD buffers and free QAT request resources.

## Required State Ownership

The current gzip wrapper cannot be reused directly for async operation because
`ZFS_COMPRESS_WRAP_DECL()` borrows and returns ABD buffers inside a synchronous
function call. Async QAT must keep those buffers and the QAT metadata alive
across the callback boundary.

The async request object needs to own:

- Source ABD reference or borrowed source buffer.
- Destination ABD and borrowed destination buffer.
- Source length, destination limit, compression level, and selected QAT session.
- QAT `CpaBufferList`, `CpaFlatBuffer`, metadata, and scratch/coalescing buffers.
- QAT result fields: API status, `dc_results.status`, produced bytes, checksum,
  overflow/incompressible status, and failure reason.
- Completion state for the ZIO stage to distinguish `not submitted`,
  `submitted`, `complete`, and `done`.

The `zio_t` needs a pointer to the in-flight QAT compression state or an
equivalent side-table entry. A direct `zio_t` field is the simplest and safest
to reason about in this fork, but it changes internal structure layout and must
be reviewed carefully.

## Suggested Implementation Slices

### Slice 1: Async API Skeleton

Add an internal QAT gzip async API in `qat_compress.c` and `qat.h` without
changing runtime behavior:

```text
qat_dc_compress_submit_async(...)
qat_dc_compress_finish_async(...)
qat_dc_compress_cancel_async(...)
```

Keep the existing `qat_compress()` synchronous API intact and implement it using
the same lower-level setup/submit/finish helpers where practical. This reduces
semantic drift between sync and async paths.

Add kstats for:

- async submit attempts
- async submit failures
- async completions
- async software fallbacks
- async resume count
- async cancel/free count

### Slice 2: ZIO Integration Behind A Disabled Default

Add an experimental module parameter, default disabled:

```text
zfs_qat_dc_async=0
```

When disabled, behavior remains exactly the current synchronous path.

When enabled, only gzip write compression should use async QAT. Decompression,
checksum, and encryption remain unchanged.

Initial eligibility should be conservative:

- `compress` is one of the gzip levels.
- `qat_dc_compress_use_accel(lsize)` is true.
- The write is not raw-compress.
- Source and destination sizes are inside already-tested bounds.
- Software fallback remains available.

### Slice 3: Resume And Fallback

Modify `zio_write_compress()` to support three async states:

```text
none       -> normal current behavior
submitted  -> return NULL after QAT submit
complete   -> finish QAT result or run software fallback
```

On first submit, rewind the stage before returning:

```text
zio->io_stage = ZIO_STAGE_ISSUE_ASYNC;
return (NULL);
```

The resumed execution advances back to `ZIO_STAGE_WRITE_COMPRESS`, observes
`complete`, finalizes the result, clears async state, and continues.

### Slice 4: Host Smoke

Use `pve.drewnet.online` only after a successful DKMS build and initramfs update.
Do not install dracut packages.

Smoke test matrix:

```text
zfs_qat_dc_async=0 baseline smoke
zfs_qat_dc_async=1 async smoke
RECORDS="128K"
MODES="qat sw"
ITERS=1
JOBS=1
VERIFY_MODE=sw
```

Required pass criteria:

- No pool errors.
- `cmp` passes.
- QAT async submit/completion counters move.
- `dc_fails=0` or failures fall back to software without data errors.
- Host can be restored to `zfs_qat_dc_async=0`.

### Slice 5: Benchmark

Only after smoke passes:

```text
VERIFY_MODE=sw ITERS=3 JOBS=1 RECORDS="128K 256K 1M" MODES="qat sw"
VERIFY_MODE=sw ITERS=3 JOBS=4 RECORDS="128K 256K 1M" MODES="qat sw"
```

Compare against the current best-case level 1 result:

```text
/root/zfs-qat-phase4-level1-bestcase-jobs1-20260516.csv
/root/zfs-qat-phase4-level1-bestcase-jobs4-20260516.csv
```

## Primary Risks

- Advancing the ZIO pipeline before compressed data is finalized would corrupt
  downstream encryption, checksum, allocation, or physical write behavior.
- Returning ABD buffers before QAT completion would create use-after-free or data
  corruption risk.
- Running too much logic in the QAT callback may violate callback-context
  assumptions or block QAT driver progress.
- Software fallback after async QAT failure needs source data to still be
  available.
- Reexecute, suspend, nopwrite, dedup, embedded-data, and rewrite paths must not
  observe a half-compressed state.

## Acceptance Criteria

- Default behavior is unchanged with `zfs_qat_dc_async=0`.
- Async mode is opt-in and gzip-compression-only at first.
- Every QAT async failure preserves software fallback or stores the block
  uncompressed exactly as the synchronous path does.
- Data verification passes for every benchmark row.
- Kstats prove async submit/completion/resume occurred.
- The async path improves wall-clock latency or throughput versus the level 1
  best-case baseline before it is considered for default policy.

## Recommendation

Proceed with Slice 1 and Slice 2 only as the first implementation pass. Do not
attempt to switch defaults or remove the synchronous path until async smoke and
benchmark evidence show a real benefit.

## Initial Implementation Smoke

Run date: 2026-05-16.

The first implementation pass added an opt-in callback-driven QAT gzip write
compression path behind:

```text
zfs_qat_dc_async=0
```

Default behavior remains synchronous. Async is only eligible when gzip
compression is selected, QAT compression acceleration is enabled, and both
experimental source and destination coalescing are disabled. Async QAT failures
force software gzip fallback rather than re-entering synchronous QAT gzip.

Validation host:

```text
pve.drewnet.online
kernel: 7.0.0-3-pve
zfs srcversion: 8A5AE0428912589FC23A222
```

Smoke CSVs:

```text
/root/zfs-qat-phase4-async-off-smoke-r2-20260516.csv
/root/zfs-qat-phase4-async-on-smoke-r2-20260516.csv
```

Smoke settings:

```text
VERIFY_MODE=sw
ITERS=1
JOBS=1
RECORDS=128K
MODES=qat
zfs_qat_cpa_dc_level=4
zfs_qat_dc_max_buf_size=1048576
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
```

Smoke result:

```text
mode       async elapsed_ms MiB_s  ratio  dc_fails async_submits submit_fails completions fallbacks verify
qat        0     908.370    200.89 17.11x 0        0             0            0           0         yes
qat        1     718.856    253.85 17.03x 0        1460          529          931         529       yes
```

Result:

- The default-off path still loads with `zfs_qat_dc_async=0`.
- The async path resumed ZIOs from QAT callbacks: `dc_compress_async_completions=931` and `dc_compress_async_resumes=931`.
- The async smoke was `20.9%` faster by elapsed time than the default-off smoke for this single 128K row.
- Correctness passed with software read verification and `zpool status -x` remained healthy.
- Async submit failures were high (`529 / 1460` submits), but each failure fell back to software gzip without `dc_fails` or data mismatch.
- The host was restored to `zfs_qat_dc_async=0` after the smoke.

Next work should tune async submit pressure and run the full 128K/256K/1M,
jobs 1/jobs 4 matrix before treating the async path as a proven performance
direction.

## Retry/Backoff Tuning Checkpoint

Run date: 2026-05-16.

The initial smoke showed high async submit fallback counts. Follow-up
instrumentation split submit failures by QAT status and confirmed the failures
were `CPA_STATUS_RETRY`, not resource exhaustion or generic errors.

New async-only controls:

```text
zfs_qat_dc_async_submit_retries=8
zfs_qat_dc_async_retry_us=100
```

The default remains `zfs_qat_dc_async=0`; these controls only matter when the
experimental async path is enabled.

128K single-job retry probes:

```text
retries retry_us elapsed_ms submit_fails retry_success final_retry_fails
0       50       864.967    411          0             411
2       50       709.949    509          149           509
8       50       703.007    426          345           426
8       10       782.450    465          253           465
8       100      699.131    360          414           360
16      50       775.053    326          493           326
32      50       878.327    167          686           167
```

The best single-row result was `8` retries with `100 us` backoff. Larger retry
counts reduced final fallback counts but increased elapsed time.

One-iteration async candidate matrix with `8/100`:

```text
jobs record async_ms sw_ms   async_vs_sw async_MiB_s sw_MiB_s async_fallbacks verify
1    128K   780.894  709.321 +10.1%      233.68      257.26   331             yes
1    256K   709.884  649.872 +9.2%       257.06      280.80   16              yes
1    1M     674.051  563.668 +19.6%      270.72      323.74   0               yes
4    128K   1147.142 1178.060 -2.6%      636.30      619.60   3145            yes
4    256K   1041.147 1079.694 -3.6%      701.08      676.05   1548            yes
4    1M     1236.152 960.369  +28.7%     590.48      760.05   35              yes
```

Negative `async_vs_sw` means async QAT was faster. This result supports
continued async work for concurrent 128K/256K writes, but it does not yet meet
the broader throughput and latency goals. Single-job rows and 1M records still
favor software gzip.

## In-Flight Cap Checkpoint

Run date: 2026-05-16.

The next implementation pass added:

```text
zfs_qat_dc_async_max_inflight=96
```

The cap avoids filling QAT's submit path. When the cap is reached, the block
uses software gzip directly instead of preparing a QAT request that is likely to
return `CPA_STATUS_RETRY`.

Jobs=4 cap sweep:

```text
cap record async_ms MiB_s  completions cap_skips submit_fails verify
32  128K   1033.719 706.12 1335        4505      0            yes
32  256K   964.506  756.79 687         2233      0            yes
64  128K   1063.828 686.13 1535        4305      0            yes
64  256K   936.620  779.32 759         2161      0            yes
96  128K   1056.668 690.78 1402        4438      0            yes
96  256K   924.537  789.51 687         2233      0            yes
192 128K   1072.564 680.54 1697        4143      0            yes
192 256K   972.536  750.54 777         2143      0            yes
256 128K   1040.878 701.26 1831        4009      0            yes
256 256K   954.047  765.09 825         2095      0            yes
512 128K   1096.017 665.98 1935        3727      178          yes
512 256K   982.047  743.27 1082        1680      158          yes
```

Cap `96` was selected as the current balanced default for async-enabled mode. It
is not a default-on policy: `zfs_qat_dc_async=0` remains the default.

Cap-96 comparison:

```text
jobs record async_ms sw_ms   async_vs_sw verify
1    128K   753.630  692.052 +8.9%       yes
1    256K   682.103  601.603 +13.4%      yes
1    1M     599.406  573.096 +4.6%       yes
4    128K   1073.745 1172.390 -8.4%      yes
4    256K   927.778  956.636  -3.0%      yes
4    1M     876.489  892.719  -1.8%      yes
```

Result:

- Admission control removes submit failures in the measured jobs=4 cap sweep.
- Async QAT with cap `96` is useful under concurrent write pressure as an
  adaptive hybrid QAT/software policy.
- Software still wins the single-job rows, so the next implementation target is
  policy gating rather than defaulting async for every gzip write.

Important caveat:

- Cap-skipped blocks use software gzip directly. Rows with nonzero
  `dc_compress_async_cap_skips_delta` are therefore not pure-QAT measurements.
  In the cap-96 jobs=4 `128K` row, `1402` QAT completions and `4438` cap skips
  means `24.0%` QAT and `76.0%` software fallback.

Small-record follow-up:

```text
Source CSVs:
/root/zfs-qat-phase4-async-cap96-small-jobs1-20260517.csv
/root/zfs-qat-phase4-async-cap96-small-jobs4-20260517.csv

jobs record async_ms sw_ms    async_vs_sw qat_share
1    8K     1668.786 1541.165 +8.3%       100.0%
1    16K    1299.723 1160.976 +12.0%      99.8%
1    32K    1105.450 1169.066 -5.4%       57.3%
1    64K    976.121  837.671  +16.5%      41.1%
4    8K     2422.484 2344.700 +3.3%       32.7%
4    16K    1791.121 1617.184 +10.8%      25.6%
4    32K    1790.974 1709.059 +4.8%       35.2%
4    64K    1339.324 1219.847 +9.8%       26.0%
```

The smaller-record matrix does not support treating cap-96 as a general
performance win. Software gzip won every four-job row and three of four
single-job rows. The only winning row, single-job `32K`, was already a mixed
QAT/software row.
