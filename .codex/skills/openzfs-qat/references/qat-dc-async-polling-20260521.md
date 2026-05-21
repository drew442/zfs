# QAT DC Async Compression - 2026-05-21

## Goal

Make async QAT DC compression a complete operator-facing feature for the
current QAT 1.x scope. Async requests must complete and resume the suspended
`zio` in both interrupt and polling modes. If an accepted async request times
out, ZFS must avoid pool lockup, avoid late-DMA corruption, and fall back to
software when it can do so safely.

This feature covers async compression only. QAT decompression remains
synchronous.

## Completed Behavior

Async compression is no longer treated as experimental in the module parameter
descriptions.

The completed behavior is:

- Normal async completion resumes the suspended `zio` by callback in interrupt
  mode or by the central poller in polling mode.
- Async requests are tracked while active so the watchdog can find accepted
  async work during no-progress stalls.
- On watchdog timeout, active async requests are marked timed out and their
  `zio` is resumed. The `zio` abandons the QAT request, keeps the QAT source and
  destination buffers reserved for any late DMA, clears `io_qat_dc_async`, and
  falls back to software gzip using the original ABD source.
- If QAT later completes an abandoned request, the late callback releases the
  retained source/destination buffers and QAT request state without resuming the
  already-fallen-back `zio`.
- If QAT never completes an abandoned request, the retained source/destination
  buffers remain intentionally reserved rather than being freed and potentially
  reused under late DMA.
- `zfs_qat_dc_quarantine_dst` still disables async. That is now a policy choice:
  synchronous quarantine is a separate safety mode, while async has its own
  retained-buffer timeout fallback.
- Async cap/backpressure can intentionally fall back to software when QAT
  capacity is exhausted. Benchmark interpretation must distinguish those cap
  skips from hardware-QAT completions.

## Completion Evidence

The feature-complete implementation was built as DKMS and loaded on
`pve.drewnet.online` with ZFS srcversion `8E806B0F2ECAB0E00043522`.

Artifacts:

- `.codex/skills/openzfs-qat/artifacts/zfs-qat-async-complete-interrupt-128k1m-jobs1-20260521.csv`
- `.codex/skills/openzfs-qat/artifacts/zfs-qat-async-complete-poll-128k1m-jobs1-20260521.csv`
- `.codex/skills/openzfs-qat/artifacts/zfs-qat-async-complete-timeout-1m-jobs8-20260521.csv`

Results:

- Interrupt mode, `128K` and `1M`, `JOBS=1`, `ITERS=3`: all rows had
  `sha_ok=yes`; async produced 4,838 completions/resumes, zero watchdog
  timeouts, and zero late completions.
- Polling mode, `128K` and `1M`, `JOBS=1`, `ITERS=3`: all rows had
  `sha_ok=yes`; async produced 4,865 completions/resumes, 98,136 poll calls,
  zero poll failures, zero watchdog timeouts, and zero late completions.
- Induced timeout, interrupt mode, `1M`, `JOBS=8`, `zfs_qat_dc_watchdog_timeout_ms=1`:
  `sha_ok=yes`; 91 async request timeouts were observed, 73 requests recovered
  through software fallback, 73 late completions released retained async state,
  async in-flight returned to zero, and QAT health was reset to `1`.
- Host restore completed after validation: QAT kernel DC instance polling reset
  to `0`, ZFS QAT parameters reset to `profile` defaults, watchdog health `1`,
  retained quarantine count/bytes `0`, post-reboot poll calls `0`, and async
  in-flight `0`.

## Existing Async Shape

The OpenZFS write path uses `zio_qat_dc_async_write()`:

```text
borrow source ABD buffer
borrow destination ABD buffer
qat_dc_compress_async_submit(...)
store async state on zio
arm request
suspend zio at ZIO_STAGE_ISSUE_ASYNC
QAT callback -> zio_interrupt(zio)
finish request and return/copy ABD buffers
```

The async QAT request owns heap-allocated request state, QAT buffer lists,
callback context, and mapped page arrays until `qat_dc_compress_async_finish()`
cleans it up. If the watchdog abandons the request, the request also retains
the borrowed source and destination ABD buffers through a cleanup callback until
QAT completes late.

The existing central poller already polls while aggregate QAT DC in-flight work
is nonzero. Async submit calls `qat_dc_inflight_enter()`, and async callback
calls `qat_dc_inflight_exit()`, so async requests can wake and use the same
central poller as synchronous requests.

## Timeout Shape

The timeout path is intentionally conservative:

- The watchdog does not free QAT buffers for a timed-out accepted request.
- The resumed `zio` does not reuse the QAT destination ABD. It performs software
  gzip into a new ABD.
- The abandoned QAT request no longer has a `zio` resume callback, so a late QAT
  completion cannot re-enter an already-completed fallback `zio`.
- The late callback cleans retained async state from a taskq so release work is
  not forced into the QAT callback path.

## Rationale

Polling changes how the QAT driver delivers callbacks; it does not require a
different `zio` resume mechanism if a central poller is driving
`icp_sal_DcPollInstance()`. The unsafe earlier shape was per-request waiter
polling. Async has no waiter polling; it only needs the central poller to run
while async requests are in flight.

Quarantine is different. Synchronous quarantine writes QAT output to a private
destination and copies successful output into the final destination. Async
already borrows a copied source and separate compressed-output ABD, so its
timeout fallback can retain those buffers and proceed with software fallback
without enabling synchronous quarantine.

## Implemented Changes

- Polling no longer disables async compression. The central poller can drive
  async callbacks because accepted async requests participate in aggregate QAT
  DC in-flight accounting.
- Async requests are added to an active list after successful QAT submission
  and removed on normal completion, late completion, submit failure, or cancel.
- The watchdog can now resume accepted async requests that exceed
  `zfs_qat_dc_watchdog_timeout_ms`.
- The `zio` async state keeps the copied source ABD so software fallback can
  safely recompress from the original bytes after a QAT timeout.
- Timed-out async requests are abandoned rather than freed. Their QAT source
  and destination buffers remain owned by the request until a late callback
  releases them.
- Late callbacks for abandoned async requests count late completion, release
  retained request state from `system_taskq`, and do not resume the fallback
  `zio` a second time.
- Async module parameter descriptions no longer label async or async
  max-inflight as experimental.
- The benchmark harness always includes `1M` when `RECORDS` is overridden, so
  async validation now covers the large-record path.

## Completion Criteria

- Normal interrupt-mode async compression completes with `sha_ok=yes`,
  nonzero async submit/completion/resume counters, zero watchdog timeouts, and
  async in-flight returning to zero.
- Normal polling-mode async compression completes with `sha_ok=yes`, nonzero
  async submit/completion/resume counters, nonzero poll calls, zero poll
  failures, zero watchdog timeouts, and async in-flight returning to zero.
- Induced async timeout resumes affected `zio`s, abandons timed-out QAT
  requests, falls back to software gzip, preserves `sha_ok=yes`, counts timeout
  and recovery events, and releases retained async state on late completion.
- Host configuration is restored to default interrupt/profile mode after
  polling and timeout validation.

## Status

- 2026-05-21: Async compression promoted to completed feature for the current
  QAT 1.x scope after implementing watchdog-driven async timeout fallback,
  retained late-completion cleanup, interrupt-mode validation, polling-mode
  validation, and induced-timeout validation.
- 2026-05-21: Implemented and smoke-tested on `pve.drewnet.online` with two
  DH895XCC cards, QAT DC kernel instances in poll mode, `zfs_qat_dc_poll=1`,
  `zfs_qat_dc_poll_interval_us=10`, `zfs_qat_dc_async=1`, and
  `zfs_qat_dc_quarantine_dst=0`.
- Initial polling validation artifact:
  `.codex/skills/openzfs-qat/artifacts/zfs-qat-async-poll-smoke-128k-jobs1-20260521.csv`.
  The corrected smoke row had `sha_ok=yes`, 1,460 async submits, 1,460 async
  completions, 1,460 async resumes, zero async fallbacks, 15,888 poll calls,
  zero poll failures, zero watchdog request timeouts, and watchdog health `1`.
- Initial comparison artifacts:
  `.codex/skills/openzfs-qat/artifacts/zfs-qat-async-poll-on-64k128k-jobs1-20260521.csv`
  and
  `.codex/skills/openzfs-qat/artifacts/zfs-qat-async-poll-off-64k128k-jobs1-20260521.csv`.
  With async enabled, 128K averaged 749.078 ms versus 764.994 ms with async
  disabled. At 64K, the current balanced async cap policy skipped async QAT and
  fell back to software for the async path, so 64K async-enabled rows are not a
  hardware-async performance result.
- Host restore completed after validation: QAT kernel DC instance polling reset
  to `0`, ZFS QAT parameters reset to `profile` defaults, watchdog health `1`,
  retained quarantine count/bytes `0`, and post-reboot poll/async counters `0`.
