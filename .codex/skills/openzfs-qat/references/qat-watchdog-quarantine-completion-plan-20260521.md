# QAT Watchdog And Quarantine Completion Plan - 2026-05-21

## Target End State

The practical end state is a synchronous QAT DC compression path that can
survive a QAT completion blackhole without corrupting the final ZFS destination
buffer and without sending new work into a failed QAT path.

Feature complete does not mean every QAT failure can be repaired in place. It
means the common recoverable case is handled safely:

```text
QAT accepts compressed write -> completion does not arrive by timeout
watchdog marks QAT DC failed -> final ZFS destination remains untouched
software gzip fallback writes final destination -> ZFS I/O can complete
late QAT DMA/read, if any, can only touch retained private QAT memory
```

The unrecoverable case remains explicit:

```text
QAT accepts request that owns a final destination buffer
completion does not arrive
watchdog fails QAT closed
caller may remain blocked until reboot/module/device reset
```

## Current State

- The watchdog is aggregate-only. It detects global no-progress and disables
  new QAT DC submissions.
- The watchdog does not recover accepted requests.
- Quarantine mode isolates synchronous compression output into a private
  destination buffer and copies successful output to the final destination.
- Quarantine mode does not yet implement timeout, fallback, late completion
  handling, or retained timed-out-buffer ownership.
- Destination quarantine alone is not sufficient for safe timeout fallback. The
  accepted request's source buffer, result storage, buffer lists, metadata, and
  callback context must also remain valid until QAT completes or device/module
  teardown makes late access impossible.
- Quarantine and polling disable experimental async compression.

## Feature Complete Watchdog Requirements

- Keep the existing aggregate no-progress detector as the cheap broad failure
  gate.
- Add request-local timeout waiting for synchronous requests.
- Ensure request-local timeout uses the same profile/manual timeout value as
  the aggregate watchdog unless a later separate parameter is justified.
- Mark QAT DC runtime failed when any request-local timeout occurs.
- Refuse new QAT DC submissions after runtime failure.
- Keep manual re-enable blocked while accepted requests or retained quarantine
  buffers still exist.
- Count request-local timeouts separately from aggregate watchdog stalls.
- Count requests that are locally recovered through software fallback.
- Count requests that time out but cannot be recovered because they used a
  direct destination or decompression path.
- Count late completions that arrive after local timeout.
- Preserve interrupt and polling behavior. Polling changes completion delivery,
  not the timeout/recovery state machine.
- Avoid hot-path overhead beyond one timestamp per accepted synchronous request
  and simple state transitions.

## Feature Complete Quarantine Requirements

- Keep QAT compression output in private memory until QAT completion and result
  validation succeed.
- Use private source memory for any request that can be locally timed out and
  recovered. Late QAT DMA reads must not target ZFS-owned source pages after
  the caller has returned.
- Keep result storage, buffer lists, metadata, callback context, source memory,
  and destination memory valid after timeout until late completion or teardown.
- On successful QAT completion, copy validated compressed output to the final
  ZFS destination and release the private buffer normally.
- On timeout before completion, leave the final ZFS destination untouched.
- Retain the private destination buffer after timeout because late QAT DMA may
  still target it.
- Never free or reuse a timed-out private destination buffer until a late
  completion is observed or module/device teardown makes DMA impossible.
- Software-fallback only the quarantined synchronous compression case.
- Do not attempt same-buffer fallback for direct-destination compression.
- Do not attempt accepted-request fallback for decompression.
- Keep experimental async disabled while quarantine is effective until async
  ownership and `zio` resume semantics are redesigned.
- Bound retained-memory growth enough to prevent a repeated QAT failure from
  consuming unbounded memory.
- Expose retained-buffer count and retained-byte counters.
- Expose timeout, fallback, unrecoverable, late-completion, and retained-release
  counters.

## Implementation Steps

1. Add this plan and keep it updated with implementation status.
2. Add kstat fields and benchmark columns for the new recovery state.
3. Add a request state object for synchronous QAT DC operations.
4. Convert synchronous waits to timed waits controlled by
   `zfs_qat_dc_watchdog_timeout_ms`.
5. On timeout, mark QAT DC runtime failed and complete the local wait path.
6. For quarantined compression timeout, retain QAT-owned private source,
   destination, result, metadata, and callback memory, then return a failure
   that permits the existing software fallback path.
7. For direct-destination compression and decompression timeout, fail closed and
   keep current unrecoverable semantics.
8. Add late-completion handling that records late completion and releases
   retained quarantine buffers when safe.
9. Validate normal-path QAT, quarantine enabled, watchdog clean counters, and
   software fallback correctness on `pve.drewnet.online`.
10. Validate induced timeout behavior on the disposable test host.
11. Benchmark overhead after correctness is proven.

## Status

- 2026-05-21: Plan created. Implementation not yet started.
