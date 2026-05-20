# QAT DC Failure Availability And Recovery - 2026-05-20

## Summary

Yes. The availability summary applies to both interrupt and polling delivery
once QAT has accepted a DC request.

Interrupt versus polling changes how completions are delivered. It does not
change the ZFS-level dependency that an accepted QAT request must eventually
complete before a synchronous waiter can return or an asynchronous `zio` can
resume. If completion delivery stops after acceptance, affected ZFS I/O can
remain outstanding indefinitely in either mode.

The expected consequence of a pure no-completion failure is availability loss,
not pool-wide corruption. The more serious corruption class is successful but
incorrect compressed output that ZFS accepts and writes.

## Delivery Modes

Interrupt mode:

- QAT DC instances are configured for interrupt-delivered response processing.
- ZFS submits a request and waits for the normal callback/completion path.
- If the interrupt/driver/callback path never delivers completion after QAT
  accepted the request, the synchronous caller remains blocked.

Polling mode:

- QAT DC instances are configured with `DcNIsPolled = 1`.
- ZFS must call `icp_sal_DcPollInstance()` for the selected DC instances.
- This fork uses one central `zfs_qat_dc_poll` kernel thread; per-request
  waiter polling stranded requests during testing.
- If polling is not running, polling fails to make progress, or the QAT driver
  state does not match ZFS polling expectations, accepted requests can remain
  outstanding.

The current implementation fails closed at QAT DC init when
`cpaDcInstanceGetInfo2()` reports an `isPolled` state that does not match the
effective `zfs_qat_dc_poll` setting. This protects both unsafe mismatches:

- QAT configured for poll delivery while ZFS is not polling.
- QAT configured for interrupt delivery while ZFS expects poll delivery.

## Mode Matrix

| Mode | Current behavior if an accepted request never completes |
|---|---|
| Interrupt + synchronous compression | Waiting thread remains blocked. Enough stranded requests can make the affected dataset or pool write path appear wedged. |
| Interrupt + experimental async compression | Callback/resume never happens, so the `zio` remains outstanding and can block pipeline progress. |
| Polling + synchronous compression | Waiting thread remains blocked if the central poller cannot obtain completion progress. Config mismatches now fail closed during QAT DC init. |
| Polling + experimental async compression | Disabled by design. The async path has not been redesigned around poll-driven completion and `zio` resume. |
| Quarantined destination + synchronous compression | Final ZFS destination remains isolated until QAT success, but this first version still has no per-request timeout, cancel, or accepted-request fallback. |
| Quarantined destination + experimental async compression | Disabled by design while quarantine mode is effective. |

## Watchdog And Quarantine Impact

The QAT DC watchdog is an aggregate no-progress detector. It starts one sleeping
`zfs_qat_dc_watchdog` kernel thread and disables new QAT DC submissions if
tracked in-flight requests make no completion progress for longer than the
configured timeout.

The watchdog does not rescue requests already accepted by QAT. That limitation
is intentional. In the direct-destination path, QAT may still perform late DMA
into the output buffer after ZFS has timed out locally. Software fallback into
that same buffer would be unsafe.

Quarantined destination mode moves QAT compression output into a private buffer
and copies successful output into the final ZFS destination only after QAT
completion and size validation. This is the first required primitive for a
future accepted-request timeout/fallback design. It does not yet implement:

- A per-request timeout.
- A QAT cancel or drain operation.
- A retained timed-out-buffer quarantine list.
- Software fallback after QAT has accepted a request.

Without a future accepted-request recovery path, a true completion blackhole can
still require module reset or reboot to restore availability.

## Data Loss And Corruption Expectations

For a pure no-completion failure, the primary expected outcome is unavailable
I/O:

- Already committed transaction groups should remain consistent because ZFS is
  copy-on-write.
- In-flight asynchronous writes that were not committed can be lost after reboot
  in the same way they can be lost after an unclean shutdown.
- Acknowledged synchronous writes should be protected by the ZIL/SLOG path and
  replayed during import if needed.
- The pool may need reboot or module reset if kernel threads remain blocked on
  QAT completions.

The more serious integrity risk is not the blackhole itself. It is QAT returning
successful but wrong compressed data that ZFS then checksums and writes. In that
case the checksum protects the bytes ZFS wrote, even if those bytes represent
incorrect compressed output. Decompression errors, Adler checks, or QAT
Compress-and-Verify may catch some failures, but normal scrub/redundancy cannot
repair every replica if the same bad payload was accepted and written
everywhere.

## ZFS Recovery And Repair Features

Useful ZFS features after a QAT-related availability failure:

- Copy-on-write transaction groups let import choose the last consistent txg.
- ZIL/SLOG replay can recover acknowledged synchronous writes.
- End-to-end checksums detect later disk/media corruption after write.
- Mirrors, RAIDZ, and ditto blocks can repair checksum mismatches when a good
  replica exists.
- Scrub can find and repair checksum mismatches across redundant replicas.
- Snapshots preserve older logical versions and can support manual rollback.

Limits:

- These features do not unblock an in-kernel QAT request that has been accepted
  but never completes.
- They do not make same-buffer software fallback safe while late QAT DMA remains
  possible.
- They may not detect semantic corruption if ZFS checksummed and wrote already
  corrupted compressed bytes.
- They cannot repair corruption when all replicas contain the same bad payload.

## Operator Position

If an operator knowingly disables watchdog and quarantine behavior, the likely
failure posture for a QAT completion blackhole is that affected I/O can block
until reboot or module/device reset. That posture is similar in interrupt and
polling mode after request acceptance.

The practical safety baseline should therefore remain:

- Keep QAT DC init fail-closed on interrupt/polling mode mismatch.
- Keep the watchdog enabled by default to stop new QAT DC submissions after
  aggregate no-progress.
- Treat quarantine mode as the foundation for future accepted-request fallback,
  not as a complete recovery mechanism yet.
- Treat successful-but-wrong output as a separate integrity problem requiring
  QAT verification, ZFS validation, and conservative fallback policy.
