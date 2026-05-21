# QAT DC Async Polling - 2026-05-21

## Goal

Enable experimental async QAT DC compression while QAT DC instances use poll
completion delivery. The target is correctness first: async requests must
complete, resume the suspended `zio`, and fall back to software on normal QAT
compression failures without stranding writes.

This phase does not make async compression timeout-recoverable. The completed
watchdog/quarantine recovery path is synchronous-only.

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

- 2026-05-21: Plan created. Implementation not yet started.

