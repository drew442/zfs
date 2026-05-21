# QAT DC Async Polling - 2026-05-21

## Goal

Enable the current async QAT DC compression path while QAT DC instances use poll
completion delivery. The target is correctness first: async requests must
complete, resume the suspended `zio`, and fall back to software on normal QAT
compression failures without stranding writes.

This phase does not make async compression timeout-recoverable. The completed
watchdog/quarantine recovery path is synchronous-only.

## Why It Is Still Experimental

The module parameter still describes async compression as experimental because
the normal completion path works, but the feature does not yet have the same
safety, policy, and validation coverage expected for a completed operator-facing
feature.

Current gaps:

- Async accepted-request timeout handling is not recoverable.
- Quarantined destination fallback is synchronous-only, so async remains
  disabled when `zfs_qat_dc_quarantine_dst` is effective.
- Async cancellation/error ownership needs a dedicated review for cases where a
  `zio` is cancelled while a QAT request may still complete later.
- The async cap policy can still silently favor software fallback at smaller
  record sizes; that is acceptable behavior, but it must be explicit in profile
  documentation and benchmark interpretation.
- Validation currently covers normal async completion under interrupt and poll
  delivery, not induced QAT failure, cancellation, pool export/import, module
  unload, or broad record-size/concurrency matrices.

## Completion Criteria

Treat async compression as feature-complete when all of the following are true:

- Normal async compression validates under interrupt and poll delivery across
  the standard benchmark record-size set, including `1M`.
- Async timeout behavior is explicit and safe: either async remains fail-closed
  with documented availability-only consequences, or async uses a quarantined
  destination design that can safely fall back after accepted-request timeout.
- Any late QAT completion after timeout or cancellation cannot write into freed
  or reused ZFS-owned memory.
- Async cancellation, failure, module unload, and pool export/import paths are
  reviewed and tested enough to show request state is not leaked or freed early.
- Profile-driven defaults document when async is expected to offload QAT and
  when it may intentionally fall back to software because of cap/backpressure.
- Benchmark artifacts show `sha_ok=yes`, no watchdog request timeouts in normal
  runs, no poll failures in polling runs, clean async submit/completion/resume
  accounting, and no retained quarantine buffers after tests.
- The user-facing module parameter descriptions and docs no longer need the
  `experimental` qualifier.

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
or `qat_dc_compress_async_cancel()` cleans it up.

The existing central poller already polls while aggregate QAT DC in-flight work
is nonzero. Async submit calls `qat_dc_inflight_enter()`, and async callback
calls `qat_dc_inflight_exit()`, so async requests can wake and use the same
central poller as synchronous requests.

## Safe Initial Scope

In scope:

- Allow async compression when `zfs_qat_dc_poll` is effective.
- Keep async disabled when `zfs_qat_dc_quarantine_dst` is effective.
- Keep polling mode fail-closed through existing `isPolled` validation.
- Validate that polled async requests generate callbacks and resume `zio`.
- Validate that normal-path watchdog and retained-quarantine counters stay
  clean.

Out of scope for the first step:

- Async accepted-request timeout fallback.
- Async quarantine ownership.
- Async decompression.
- Per-request async watchdog recovery.

## Rationale

Polling changes how the QAT driver delivers callbacks; it does not require a
different `zio` resume mechanism if a central poller is driving
`icp_sal_DcPollInstance()`. The unsafe earlier shape was per-request waiter
polling. Async has no waiter polling; it only needs the central poller to run
while async requests are in flight.

Quarantine is different. Async fallback after an accepted request would require
retaining source, destination, result, callback, and `zio` ownership safely
across timeout and late completion. That is not implemented, so quarantine must
continue to disable async.

## Implementation Steps

1. Change `qat_dc_effective_async()` so polling no longer disables async.
2. Keep `qat_dc_effective_quarantine_dst()` as an async disable condition.
3. Update polling docs and source notes.
4. Build/install/reboot on `pve.drewnet.online`.
5. Configure QAT driver DC instances and ZFS for polling mode.
6. Run async polling smoke with `zfs_qat_dc_async=1`,
   `zfs_qat_dc_poll=1`, quarantine disabled, and software verification.
7. Restore QAT/ZFS config to default interrupt/profile mode.

## Success Criteria

- `sha_ok=yes`.
- `dc_compress_async_submits`, `dc_compress_async_completions`, and
  `dc_compress_async_resumes` are nonzero.
- `dc_poll_calls` is nonzero.
- `dc_watchdog_health=1`.
- `dc_watchdog_request_timeouts=0`.
- `dc_compress_quarantine_dst_retained=0`.
- Host is restored to default interrupt/profile mode after validation.

## Status

- 2026-05-21: Implemented and smoke-tested on `pve.drewnet.online` with two
  DH895XCC cards, QAT DC kernel instances in poll mode, `zfs_qat_dc_poll=1`,
  `zfs_qat_dc_poll_interval_us=10`, `zfs_qat_dc_async=1`, and
  `zfs_qat_dc_quarantine_dst=0`.
- Validation artifact:
  `.codex/skills/openzfs-qat/artifacts/zfs-qat-async-poll-smoke-128k-jobs1-20260521.csv`.
  The corrected smoke row had `sha_ok=yes`, 1,460 async submits, 1,460 async
  completions, 1,460 async resumes, zero async fallbacks, 15,888 poll calls,
  zero poll failures, zero watchdog request timeouts, and watchdog health `1`.
- Comparison artifacts:
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
