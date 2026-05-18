# QAT Performance Optimization Next Steps

Date: 2026-05-18.

Source added:

```text
Intel QuickAssist Technology Performance Optimization Guide
Document number: 330687-008
Revision: 008
Date: December 2021
URL: https://cdrdv2.intel.com/v1/dl/getContent/709209?fileName=qat-performance-optimization-guide.pdf
```

## Current Assessment

The current data says we should pivot the next optimization pass toward the QAT
driver/platform side, while keeping a small ZFS-side queue of validation and
admission-policy work.

ZFS-side work has already covered the obvious first-order issues:

- QAT compression level is configurable and profile-managed.
- Huffman type is configurable and profile-managed.
- Large-record eligibility is configurable and profile-managed.
- Async write compression exists and can be profile-managed.
- Active-DC-count cap policy is configurable and profile-managed.
- Source and destination coalescing were implemented, repeated, and left off by
  default because they did not show a broad elapsed-time win.
- Profile sweeps show only one clear elapsed-time win so far:
  `target=1M`, `profile=throughput`, `record=1M`.

The Intel performance guide points at the remaining likely bottlenecks:

- QAT polling/interrupt behavior and polling interval.
- Maximum concurrent requests and backpressure.
- Buffer-list count and alignment.
- PCIe lane width/speed.
- NUMA locality and memory bandwidth.
- Disabling unused QAT services so compression does not share internal
  resources with crypto.
- QAT driver parameter checking.

Those are mostly QAT-driver, platform, or integration-mode targets rather than
simple ZFS policy knobs.

## ZFS-Side Work Still Worth Doing

Keep these in scope because they directly affect correctness, operator policy,
or benchmark interpretation:

- Repeat `target=1M`, `profile=throughput`, `record=1M` with more iterations and
  `JOBS=8`. Do not promote new defaults from the first sweep alone.
- Add a profile-repeat benchmark note that reports QAT byte share, fallback
  share, CPU seconds per GiB, compression ratio, QAT service time, and elapsed
  latency together.
- Review whether profile admission should refuse QAT for 128K/256K when the
  selected profile values repeatedly lose on elapsed time, even if CPU is lower.
- Keep software fallback as a first-class feature. Several apparent wins are
  fallback-driven; reporting must keep QAT byte share beside elapsed time.
- Inspect payload and SGL entry alignment in the ZFS QAT path. If misalignment
  is common, measure whether a targeted alignment strategy helps before adding
  another copy/coalescing path.
- Inspect async completion behavior for avoidable synchronous waits or poor
  callback scheduling. Treat this as a narrow implementation review, not a
  rewrite unless there is direct evidence.

## QAT-Side Work To Prioritize

These should be the next optimization target because Intel's guide identifies
them as performance-sensitive and our ZFS-side tuning has mostly hit CPU-vs-
latency tradeoffs rather than broad throughput wins.

1. Verify service split and disable unused services.
   - Goal: DC-only resources for compression benchmarking.
   - Check active `/etc/dh895xcc_dev*.conf` kernel sections.
   - Confirm `NumberDcInstances`, `NumberCyInstances`, service masks, and
     actual `dc_instances` kstat.
   - Keep ZFS checksum/encryption disabled during compression benchmarking.

2. Determine polling mode and tune if supported.
   - Goal: reduce completion latency and offload cost without starving
     throughput.
   - Identify whether the QAT 4.28 CE kernel API path is using interrupt,
     polling, or a poll thread for the configured instances.
   - If polling interval is configurable for QAT 1.x kernel instances, sweep it
     against the `target=1M throughput` benchmark.

3. Check PCIe link state.
   - Goal: rule out a platform bottleneck before making more ZFS changes.
   - Record lane width/speed for both DH895XCC cards with `lspci -vv`.
   - Compare actual trained link width/speed with expected device capability
     and slot wiring.

4. Check NUMA placement without drawing broad conclusions from this host.
   - Goal: identify obvious remote-memory penalties.
   - Record each QAT device NUMA node, CPU affinity, and benchmark process CPU
     placement.
   - Because this host is single-socket EPYC with sub-socket NUMA, use this as
     hygiene only; defer conclusions to a true multi-socket host.

5. Evaluate parameter checking as an explicit QAT-driver experiment.
   - Goal: reduce IA cycles in the access layer.
   - Only test if QAT 4.28 CE exposes the documented `ICP_PARAM_CHECK` or build
     option for this driver path.
   - Treat this as an experiment requiring correctness smoke tests, not an
     automatic deployment recommendation.

6. Confirm memory allocation and alignment behavior.
   - Goal: determine whether 64-byte alignment guidance is violated enough to
     explain service cost.
   - Measure source and destination pointer alignment, SGL entry alignment, and
     segment lengths in the ZFS QAT path.
   - Do not re-enable broad coalescing by default unless it improves elapsed
     time and not just SGL count.

## Scope And Exclusion Review

Keep out of scope:

- QAT 2.0+ and Gen4-only features.
- SVM-based changes. Intel documents SVM for newer QAT generations; this
  project targets dh895x/C620.
- In-tree-driver migration as an optimization path. It does not support the
  current QAT 1.x project target well enough to justify the disruption.
- Crypto/checksum feature expansion while compression performance is still the
  primary problem.
- SHA512-family checksum offload. The current source notes explain the OpenZFS
  SHA512/256 IV mismatch.

Move into scope as QAT-side performance configuration:

- DC-only service allocation during compression benchmarks.
- Driver polling mode and polling interval, if exposed for QAT 1.x CE kernel
  instances.
- PCIe link-width/link-speed validation.
- Parameter-checking disable as a controlled experiment.
- NUMA placement as a measurement hygiene item now and a real optimization item
  once a multi-socket host is available.

Move into conditional scope:

- AES-GCM/SHA256 checksum benchmarking only after compression profile behavior
  is stable. This has merit for completeness, but not as a path to fixing gzip
  latency.
- Source/destination alignment fixes only if instrumentation shows meaningful
  misalignment and a targeted fix beats both software gzip and the current
  coalescing experiments.

No current exclusion should be broadly lifted just to chase performance. The
valuable scope expansion is QAT 1.x driver/platform tuning, not QAT 2.0 feature
work.

## Recommended Next Action

Run a QAT-side baseline collection on `pve.drewnet.online` before changing more
ZFS code:

```text
1. Capture QAT config: /etc/dh895xcc_dev*.conf relevant kernel sections.
2. Capture PCIe link state: lspci -vv for both DH895XCC cards.
3. Capture QAT device NUMA nodes and CPU topology.
4. Capture loaded QAT module parameters and service state.
5. Repeat only the current best benchmark:
   target=1M, profile=throughput, record=1M, JOBS=4 and JOBS=8.
```

If QAT-side tuning does not improve the repeat candidate, the next ZFS-side
question should be narrower: whether profile admission should prefer software
for the record sizes where QAT only saves CPU but loses elapsed time.
