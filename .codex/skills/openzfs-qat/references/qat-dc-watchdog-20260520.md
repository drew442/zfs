# QAT DC Watchdog - 2026-05-20

## Purpose

The QAT DC watchdog is a low-overhead runtime safety mechanism for cases where
QAT hardware, driver delivery, or polling configuration stops making completion
progress after ZFS has already initialized QAT DC successfully.

It is not a per-request timeout and it does not attempt software fallback for a
request already accepted by QAT. Once QAT has a destination buffer, late DMA can
still write to that buffer, so same-buffer fallback would be unsafe.

## Implementation Shape

The watchdog uses one sleeping kernel thread named `zfs_qat_dc_watchdog`.
Request hot-path accounting is intentionally small:

- Increment a single aggregate QAT DC in-flight counter on submit.
- Record the aggregate `0 -> 1` transition timestamp.
- Record completion progress when requests leave the in-flight set.
- Reuse existing submit/completion timing points where possible instead of
  adding extra clock reads per request.

The watchdog thread wakes periodically and checks aggregate state. If at least
one QAT DC request is in flight and both the oldest aggregate in-flight window
and the last-progress window exceed the configured timeout, it marks QAT DC
runtime failed and disables new QAT DC submissions.

Existing accepted requests are left to complete. This avoids unsafe output
buffer reuse but means a truly stranded request can still leave its caller
waiting. The watchdog's safety value is preventing more ZFS work from entering
a failed QAT DC path and exposing the failure state clearly.

## Module Parameters

```text
zfs_qat_dc_watchdog=profile|0|1
zfs_qat_dc_watchdog_timeout_ms=profile|integer
zfs_qat_dc_watchdog_interval_ms=profile|integer
```

Defaults:

```text
zfs_qat_dc_watchdog=profile             # effective 1
zfs_qat_dc_watchdog_timeout_ms=profile  # effective 5000
zfs_qat_dc_watchdog_interval_ms=profile # effective 250
```

Manual ranges:

```text
zfs_qat_dc_watchdog_timeout_ms: 100..3600000
zfs_qat_dc_watchdog_interval_ms: 10..60000
```

When the watchdog fires it sets `zfs_qat_compress_disable=1` and gates both
QAT compression and decompression through the internal runtime-failed flag.
Manual re-enable by writing `0` to `zfs_qat_compress_disable` is refused with
`-EBUSY` while aggregate QAT DC in-flight work remains nonzero. If all tracked
requests have drained, manual re-enable clears the runtime-failed flag and
resets watchdog progress state.

## Kstats

```text
dc_watchdog_checks
dc_watchdog_stalls
dc_watchdog_runtime_disables
dc_watchdog_last_progress_ns
dc_watchdog_last_stall_ns
dc_watchdog_health
```

`dc_watchdog_health=1` means QAT DC was initialized and the watchdog has not
marked it failed. `dc_watchdog_health=0` means the watchdog has failed QAT DC
closed for new submissions.

## Limits

- Detects global no-progress only. If one request is stranded while other QAT
  DC requests continue completing, this first version does not identify the
  single stranded request.
- Does not cancel or retry accepted QAT requests.
- Does not replace ZFS deadman behavior. ZFS deadman-style mechanisms may
  diagnose blocked pipeline progress, but they do not make late QAT DMA safe
  for same-buffer software fallback.
- The timeout must remain conservative. Too small a value can disable QAT DC
  during normal long-tail completion latency.

## Validation

Host validation on `pve.drewnet.online`:

```text
Kernel: 7.0.0-3-pve
ZFS srcversion: C9CD203DDB2E16D696938C2
QAT DC instances: 12
Delivery mode: interrupt, zfs_qat_dc_poll=profile effective 0
Watchdog params: profile/profile/profile
Watchdog thread: [zfs_qat_dc_watchdog]
```

Smoke benchmark:

```text
Artifact:
.codex/skills/openzfs-qat/artifacts/zfs-qat-watchdog-irq-smoke-128k-jobs1-20260520.csv

Command shape:
ITERS=1 RECORDS="128K" MODES="qat sw" JOBS=1 VERIFY_MODE=sw

Result:
QAT sha_ok=yes
SW sha_ok=yes
dc_watchdog_stalls=0
dc_watchdog_runtime_disables=0
dc_watchdog_health=1
dc_poll_calls=0
```

Live parameter setter smoke passed for:

```text
zfs_qat_dc_watchdog: profile -> 0 -> profile
zfs_qat_dc_watchdog_timeout_ms: profile -> 1000 -> profile
zfs_qat_dc_watchdog_interval_ms: profile -> 100 -> profile
```
