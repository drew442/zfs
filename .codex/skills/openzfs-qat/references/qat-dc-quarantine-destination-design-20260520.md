# QAT DC Quarantined Destination - 2026-05-20

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

The same ownership rule applies to the accepted request's source memory and
control structures. A fallback-capable timeout path must keep QAT-readable
source memory, result storage, buffer lists, metadata, and callback context
valid after the ZFS caller returns. Destination quarantine alone protects the
final output buffer, but it does not make source pages or stack-allocated result
state safe for late QAT access.

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

## Implemented First Version

The first implementation adds manual/profile control for synchronous QAT
compression:

```text
zfs_qat_dc_quarantine_dst=profile|0|1
```

Current profile default is effective `0`.

When effective `1`:

- Synchronous QAT compression must obtain a private destination buffer.
- QAT output is copied into the final ZFS destination only after successful QAT
  completion and size validation.
- If private destination setup fails before QAT submit, the QAT path returns
  failure and ZFS can use existing software fallback.
- Experimental async compression is disabled while quarantine mode is effective
  because async resume semantics have not been redesigned around quarantine
  ownership.
- Existing destination-coalescing allocation/copy timing counters are reused,
  and quarantine-specific counters identify quarantine activity.

Added kstats:

```text
dc_compress_quarantine_dst_requests
dc_compress_quarantine_dst_success
dc_compress_quarantine_dst_fails
dc_compress_quarantine_dst_copy_bytes
dc_compress_quarantine_dst_retained
dc_compress_quarantine_dst_retained_bytes
dc_compress_quarantine_dst_retained_released
```

This first version does not implement a per-request timeout, QAT cancel,
private retained source memory, or retained timed-out-buffer list. It is the
safe destination-ownership primitive needed before any later accepted-request
fallback work.

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

## Initial Validation

Host validation on `pve.drewnet.online`:

```text
Kernel: 7.0.0-3-pve
ZFS srcversion: 440AC0FA85D59E11EEBC115
QAT DC instances: 12
Default zfs_qat_dc_quarantine_dst: profile, effective 0
Test zfs_qat_dc_quarantine_dst: 1
```

Artifacts:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-watchdog-baseline-records-jobs1-4-8-20260520.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-quarantine-default-smoke-128k-jobs1-20260520.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-quarantine-enabled-smoke-128k-jobs1-20260520.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-quarantine-enabled-128k-jobs1-4-8-20260520.csv
```

The full normal-operation baseline used the previous 176-column harness format.
The quarantine implementation adds six benchmark columns, so quarantine
validation artifacts use 182 columns. Field-count validation passed for header
and all data rows in the new artifacts.

Quarantine 128 KiB results:

| Jobs | QAT avg ms | SW avg ms | QAT vs SW | Quarantine requests | Quarantine fails | Watchdog stalls |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 730.749 | 758.544 | -3.66% | 1,460 per iter | 0 | 0 |
| 4 | 1092.922 | 1095.480 | -0.23% | 5,840 per iter | 0 | 0 |
| 8 | 1544.149 | 1423.978 | +8.44% | 11,684-11,688 per iter | 0 | 0 |

Interpretation:

- Functional smoke passed for QAT and software verification paths.
- Every QAT compression request in quarantine mode used the private destination
  path and had matching quarantine success counters.
- No quarantine allocation/setup failures were observed.
- No watchdog stalls or runtime disables were observed.
- The first timing data does not show a clear penalty at `JOBS=1/4`, but
  `JOBS=8` regressed versus software. Treat timing as preliminary because this
  was a narrow 128 KiB validation run, not a full profile sweep.

## Recommendation

Keep quarantine mode manual-only until more data exists. Initial smoke testing
shows the mode works functionally at 128 KiB, but it adds copy/allocation cost
and should not become a profile default without a larger overhead review.

## Recovery Extension

The 2026-05-21 recovery extension makes quarantine mode the only synchronous
compression path eligible for accepted-request software fallback after a local
timeout. When quarantine is effective, the implementation now forces a private
source copy as well as a private destination buffer. On timeout, QAT-owned
source, destination, result, metadata, buffer-list, and callback memory are
retained until late QAT completion or module/device teardown. The final ZFS
destination remains untouched and the caller can fall back to software gzip.

Direct-destination compression and decompression still fail closed without
local fallback after timeout. Experimental async compression remains disabled
while quarantine is effective.
