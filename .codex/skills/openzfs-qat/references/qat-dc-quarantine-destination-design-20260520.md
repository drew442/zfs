# QAT DC Quarantined Destination Design - 2026-05-20

## Problem

The current QAT DC watchdog can disable new QAT DC submissions when aggregate
completion progress stalls, but it cannot safely software-fallback a request
that QAT has already accepted. The unsafe case is late DMA: QAT may still write
to the destination buffer after ZFS has decided to fallback in software.

For the current direct-destination path, software fallback into the same output
buffer would risk corrupting the final compressed block.

## Design Goal

Make a future fallback-capable mode possible by ensuring QAT never writes into
the final ZFS destination buffer until ZFS knows the QAT request completed
successfully.

This should be an optional safety/profile feature, not a default performance
optimization, because it adds allocation and copy cost to every QAT compression
request.

## Proposed Shape

Add a QAT DC compression mode that uses a private destination buffer for QAT
output:

```text
source buffer -> QAT -> private/quarantine output buffer
successful completion -> copy compressed bytes into final ZFS destination
timeout/failure before completion -> leave final ZFS destination untouched
```

For compression, the final ZFS destination buffer receives data only after QAT
completion status and produced length are known. If the watchdog or a later
per-request timeout marks the QAT request failed before completion, software
compression can write to the final destination because QAT does not own that
buffer.

This does not make it safe to free or reuse the quarantine buffer immediately
after a timeout. The quarantine buffer must remain valid until QAT is known not
to DMA into it. That requires one of:

- A driver/API-supported cancel or drain primitive, if available and safe.
- Retaining the quarantine buffer in a dead-request quarantine list until QAT
  device reset/module unload.
- Marking QAT DC failed and requiring module unload/reboot before releasing
  quarantined buffers from timed-out accepted requests.

The third option is conservative but simplest.

## Scope

Compression is the useful first target. Decompression is less attractive because
fallback verification paths are different and read-side latency semantics are
more sensitive.

This design should initially apply only to synchronous QAT compression. The
existing experimental async compression path would need separate ownership and
resume semantics before it can safely use this mode.

## Tunable Proposal

```text
zfs_qat_dc_quarantine_dst=profile|0|1
```

Initial effective profile:

```text
balanced: 0
latency: 0
throughput: 0
offload: 0
```

Do not enable by profile until measured. A future `safety` profile could make
this mode default if recovery behavior becomes more important than throughput.

## Expected Costs

- Extra destination allocation or private buffer reuse requirement.
- Extra copy from quarantine buffer to final ZFS destination on every QAT
  compression success.
- Higher memory pressure for large records and high in-flight counts.
- Possible worse elapsed latency even when CPU offload remains attractive.

Existing destination coalescing experiments already showed that extra copy and
allocation paths can erase buffer-list wins. This design should therefore be
treated as a safety/recovery experiment, not a likely performance win.

## Validation Plan

1. Implement only for synchronous QAT compression.
2. Add kstats for quarantine requests, successes, fallback attempts, retained
   timed-out buffers, allocation bytes, allocation time, and copy time.
3. Add benchmark harness columns before running comparisons.
4. Validate normal completion first with no induced timeouts.
5. Induce a controlled no-progress condition only on the test host and confirm
   final destination integrity.
6. Benchmark 128K, 256K, 512K, and 1M with `JOBS=1/4/8`.

Success criteria:

- No data-integrity regressions.
- Normal path watchdog counters remain clean.
- Any recovery path avoids same-buffer fallback.
- Performance cost is explicit enough to decide whether this should remain
  manual-only.

## Recommendation

Do not implement this before one more benchmark pass with watchdog columns in
place. The added harness data will confirm whether the watchdog remains
invisible during normal runs and will give a cleaner baseline for evaluating the
quarantine mode's allocation/copy overhead.
