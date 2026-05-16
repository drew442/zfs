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
