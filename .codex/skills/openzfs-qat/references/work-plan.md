# OpenZFS QAT 1.x Work Plan

This plan defines the proposed work needed to make QAT support useful, testable, and maintainable for QAT 1.x hardware in this fork.

## Goals

- Support QAT 1.x dh895x/dh895xcc and C620/C62x-class hardware through the out-of-tree QAT CE driver path.
- Make QAT-enabled DKMS builds reproducible on Proxmox 9 with kernel `7.0.0-3-pve`.
- Prove that ZFS gzip workloads actually use QAT by observing `/proc/spl/kstat/zfs/qat` counters under controlled tests.
- Fix the compression-level behavior so QAT compression is not permanently pinned to `CPA_DC_L1` when ZFS gzip levels imply stronger compression.
- Preserve software fallback and avoid data-risky behavior if QAT initialization or individual offloads fail.

## Non-Goals

- Do not add features that require QAT hardware version 2.0 or newer.
- Do not pursue Gen4-only, SVM-only, QATlib-only, or in-tree-driver-only paths unless the same behavior is valid for QAT 1.x.
- Do not install dracut packages on `pve.drewnet.online`; that host boots with initramfs.
- Do not treat TrueNAS, forum, gist, or blog results as source-of-truth performance claims.
- Do not optimize checksum or encryption before compression build/runtime behavior is proven.

## Constraints

- `pve.drewnet.online` is approved for project testing and has no production data.
- `nvme_scratch` contains real-world test data only.
- `test-hdd-pool` is dedicated to performance testing with and without QAT.
- `/usr/src/zfs-2.4.99/` may contain stale experimental patches and should be re-baselined before new implementation work.
- `/root/QAT/QAT.L.4.28.0-00004/` is a custom fork from `https://github.com/drew442/QAT.L.4.28.0-00004.git`, modified for the current Proxmox kernel.

## Phase 0: Baseline And Reproducibility

Purpose: establish a clean baseline before changing behavior.

Status: completed for the 2026-05-12 pass. See `phase-0-1-results.md`.

Work:

- Capture local repo branch, commit, and diff.
- Capture host DKMS source state under `/usr/src/zfs-2.4.99/` and `/var/lib/dkms/zfs/2.4.99/source`.
- Decide the source of truth for implementation: this repository should drive patches, and host DKMS source should be refreshed from it.
- Re-baseline `/usr/src/zfs-2.4.99/` on `pve.drewnet.online` if stale patches remain.
- Record exact DKMS rebuild commands and QAT environment variables.
- Confirm the custom QAT driver tree builds and exposes the expected kernel API artifacts.

Acceptance:

- Clean diff is understood before new patches are applied.
- DKMS source can be rebuilt from a known repo state.
- Rebuild procedure does not install or require dracut packages.

## Phase 1: Build Integration

Purpose: make QAT detection work predictably with the QAT 4.28 tree used by the project.

Status: completed for the 2026-05-12 pass. See `phase-0-1-results.md`.

Work:

- Review `config/kernel.m4`, `config/zfs-build.m4`, `scripts/dkms.mkconf`, and `module/Kbuild.in`.
- Preserve `--with-qat=PATH` and `--with-qat-obj=PATH` behavior.
- Verify QAT headers under `${ICP_ROOT}/quickassist/include`.
- Support the QAT 4.28 `Module.symvers` location observed on the host, while preserving older supported paths if still valid for QAT 1.x.
- Improve configure errors so failures identify which artifact is missing: `cpa.h`, `qat_api.ko`/`icp_qa_al.ko`, or `Module.symvers`.
- Ensure `KBUILD_EXTRA_SYMBOLS` points at the actual symbols file used by the QAT driver build.

Acceptance:

- `./configure --with-qat=/root/QAT/QAT.L.4.28.0-00004` succeeds on the host after re-baselining.
- DKMS build produces `zfs.ko` with `HAVE_QAT=1`.
- `modinfo zfs` shows dependency on `qat_api` when built with QAT.
- Non-QAT builds still compile and keep the stub path behavior.

## Phase 2: Runtime Enablement And Observability

Purpose: make QAT state obvious and controllable after module load.

Status: completed for the 2026-05-12 pass. See `phase-2-3-results.md`.

Work:

- Review `module/os/linux/zfs/qat.c`, `qat_compress.c`, `qat_crypt.c`, and `include/sys/qat.h`.
- Verify initialization behavior when compression or crypto instances are absent.
- Preserve lazy re-enable behavior through module parameters.
- Do not carry forward the abandoned `zfs_qat_deflate_depth` experimental patch.
- Add or adjust module parameters only if they are visible, documented, and safe to change.
- Document runtime commands for disable flags and kstat inspection.

Acceptance:

- `/sys/module/zfs/parameters/zfs_qat_compress_disable` reflects runtime compression enablement.
- `/proc/spl/kstat/zfs/qat` exists when QAT is compiled in.
- Failed QAT initialization does not prevent ZFS module load.
- Re-enabling after QAT service startup is either proven or explicitly documented as unsupported.

## Phase 3: Compression Level Semantics

Purpose: remove the static `CPA_DC_L1` behavior and make the QAT compression level explicit on QAT 1.x hardware.

Status: completed for the 2026-05-12 pass. See `phase-2-3-results.md`.

Original issue:

- `module/os/linux/zfs/qat_compress.c` sets `sd.compLevel = CPA_DC_L1`.
- `module/zfs/gzip.c` receives the ZFS gzip level as `n`, but calls `qat_compress()` without passing that level.
- As a result, datasets configured as `gzip-2`, `gzip-3`, or higher may still use QAT level 1 for offloaded compression.

Work:

- Verify which `CPA_DC_L*` levels are supported by dh895x/C620 with QAT 4.28.
- Use a global module parameter for compression level because the QAT compression level is a hardware/session setting.
- Keep decompression independent of compression-level selection.
- Ensure unsupported QAT levels fall back safely to a lower QAT level or software gzip.
- Document any unavoidable mismatch between ZFS gzip levels 1-9 and QAT 1.x levels.

Likely implementation shape:

- Add `zfs_qat_cpa_dc_level` as a global module parameter.
- Limit the accepted values to QAT 1.x-safe `CPA_DC_L1` through `CPA_DC_L4`.
- Keep `CPA_DC_DEFLATE`, `CPA_DC_HT_FULL_DYNAMIC`, `CPA_DC_DIR_COMBINED`, `CPA_DC_STATELESS`, `CPA_DC_ADLER32`, and `CPA_DC_FLUSH_FINAL` unless evidence requires a QAT 1.x-safe change.
- Update `man/man4/zfs.4` for any new or changed tunables.

Acceptance:

- QAT compression no longer silently uses hardcoded `CPA_DC_L1`; the global `zfs_qat_cpa_dc_level` module parameter controls the session level.
- QAT compression kstats increase during controlled writes to gzip datasets.
- Incompressible or failed QAT jobs preserve existing software fallback behavior.
- Decompression succeeds for data written before and after the change.

## Phase 4: Offload Eligibility And Performance

Purpose: tune only after correctness and observability are proven.

Status: initial pass completed for the 2026-05-12 run. Extended phase 4 work is planned because throughput and latency are both first-class requirements, and current results do not yet show QAT gzip parity with software gzip across the target matrix. See `phase-4-results.md`.

Work:

- Re-evaluate the compression offload window for QAT 1.x and ZFS record sizes.
- Measure failed offload attempts and software fallback frequency.
- Review allocation and copy costs in `qat_compress_impl()`, especially buffer-list metadata and scratch buffers.
- Evaluate whether the fixed `QAT_DC_MAX_INSTANCES = 48` and `QAT_CRYPT_MAX_INSTANCES = 48` caps are harmless for dh895x/C620 or should be made dynamic.
- Park NUMA performance conclusions until a true multi-socket QAT 1.x host is available. `pve.drewnet.online` is a single-socket EPYC 7551P system, so its reported NUMA topology is not a suitable basis for broad NUMA tuning decisions.

Acceptance:

- Any threshold or allocation change is backed by before/after host measurements.
- Performance tests record CPU cost, throughput, latency distribution, compression ratio, QAT kstats, and failure counters.
- Tuning changes do not reduce correctness or fallback safety.

## Phase 4 Extension: Throughput, Latency, And Large Records

Purpose: extend phase 4 from a threshold pass into an exhaustive performance pass that treats throughput, latency, and compression ratio as explicit optimization dimensions.

Current facts:

- QAT gzip currently offloads records from `8 KiB` through `128 KiB`.
- Records larger than `128 KiB` currently fall back to software gzip because `qat_dc_use_accel()` rejects buffers above `QAT_DC_MAX_BUF_SIZE`.
- The current `128 KiB` ceiling is an OpenZFS implementation threshold in this fork, not yet proven to be a QAT 1.x hardware hard limit.
- Local QAT 4.28 headers expose deflate block-size capability concepts above `128 KiB`, but dh895x/C620 behavior must be proven with host measurements, correctness checks, and failure counters before changing defaults.

Workstream A: repeatable benchmark harness:

- Capture throughput, elapsed time, p50/p95/p99/max latency, CPU cost, compression ratio, QAT kstats, DC failure counters, module parameters, loaded module `srcversion`, QAT driver state, dataset settings, recordsize, and source file identity for every run.
- Run both compressible and incompressible workloads.
- Compare QAT-enabled and software-only gzip on identical data and dataset settings.
- Validate written data with `cmp` or checksums, and include read-after-reboot validation for any new compressed-data format or large-record path.

Workstream B: larger-record QAT compression:

- Add an experimental global maximum such as `zfs_qat_dc_max_buf_size`, defaulting to the current safe `128 KiB`.
- Allow only bounded QAT 1.x test values at first, for example `128 KiB`, `256 KiB`, `512 KiB`, and `1 MiB`.
- Use QAT API compression-bound information where available instead of assuming the current `2 * input` intermediate-buffer rule is always sufficient for larger records.
- Avoid unsafe kernel-stack growth when supporting larger records; page-pointer arrays and metadata storage must scale without relying on large stack allocations.
- Prove behavior on compressible and incompressible inputs before raising the default above `128 KiB`.

Status: experimental enablement completed for the 2026-05-13 pass. The repo has `zfs_qat_dc_max_buf_size` with a default of `128 KiB` and opt-in values through `1 MiB`; host validation showed 256 KiB and 1 MiB records can offload on dh895xcc/QAT 4.28 with zero DC failures in the initial matrix. More throughput and latency benchmarking is still required before changing the default above `128 KiB`.

Workstream C: latency-focused measurements:

- Measure per-record compression latency across `8 KiB`, `16 KiB`, `32 KiB`, `64 KiB`, `128 KiB`, and larger experimental record sizes if enabled.
- Track queueing effects separately from service time where practical.
- Record whether allocation reuse, instance selection, or larger batches improve throughput by adding tail latency.

Status: harness implementation started. `scripts/qat-phase4-benchmark.sh` records raw iteration elapsed time, summary p50/p95/p99/max latency, throughput, CPU percentages, compression ratio, QAT kstat deltas, module settings, and `cmp` correctness for controlled write/read tests. It also supports concurrent copy/verify streams through `JOBS`.

Workstream G: async/queueing:

- Do not wrap the existing synchronous `qat_compress()` helper in a taskq and call that sufficient; it still blocks a worker on each QAT request and adds dispatch overhead.
- Split QAT gzip compression into submit and finish phases so `ZIO_STAGE_WRITE_COMPRESS` can suspend after QAT submission and resume from the QAT callback.
- Keep the async path opt-in at first, with a disabled default such as `zfs_qat_dc_async=0`.
- Keep software fallback available after async QAT failure by retaining source data until the compression stage has finalized.
- Do not advance to encryption, checksum generation, allocation, or physical I/O until the async compression result has been finalized.

Status: design spike documented on 2026-05-16. See `phase-4-async-queue-spike.md`.

Workstream D: allocation and metadata reuse:

- Continue reducing allocation and mapping overhead in `qat_compress_impl()`.
- Prefer non-serializing reuse strategies, such as per-CPU or per-instance multi-slot pools, over a single mutex-protected workspace.
- Do not keep the abandoned serialized per-instance workspace approach unless new measurements show it no longer regresses latency or throughput.

Status: initial non-serializing reuse implemented for the 2026-05-13 pass. The compression path now has a small lock-free per-instance pool for QAT buffer-list metadata and list storage, with fallback to per-request allocation when slots are busy. Host validation showed reuse hits and misses under the smoke workload, so this reduces but does not eliminate allocation pressure.

2026-05-27 update: detailed async local timing stats are now profile-gated by
`zfs_qat_dc_timing_stats`, defaulting to off through `profile`. This removes
nonessential timestamp/stat accounting from the normal async request path while
keeping the detailed counters available for focused benchmark runs.

2026-05-27 update: the same timing gate now covers synchronous compression
local timing. This removes detailed request setup, submit, cleanup, allocation,
and compression-bound timing from the default `128K` balanced compression path
unless `zfs_qat_dc_timing_stats=1` is set for measurement.

2026-05-27 update: aggregate compression request-shape counters now follow
`zfs_qat_dc_shape_stats` instead of remaining always-on. Profile/default mode no
longer pays source/destination/scratch/max-buffer diagnostic stat updates on
each QAT compression request; set `zfs_qat_dc_shape_stats=1` for request-shape
benchmark attribution.

2026-05-28 update: compression request-path diagnostic counters now follow
`zfs_qat_dc_shape_stats` as well. Profile/default mode no longer pays buffer
reuse hit/miss, page-array stack/heap/slot, or request-slot reuse stat updates
on each QAT compression request; set `zfs_qat_dc_shape_stats=1` for request-path
benchmark attribution.

2026-05-28 update: the phase 4 benchmark harness now reports derived
admission-attribution fields that separate profile size-policy skips,
runtime-disabled/uninitialized skips, async cap skips, and async failure
fallback. Eligibility-derived attribution requires `zfs_qat_dc_shape_stats=1`;
normal profile/default runs intentionally leave those detailed fields as `na`.

Workstream E: QAT instance caps:

- Expose init-time module parameters for maximum DC and crypto instances, with defaults of `48` to preserve current behavior.
- Candidate names are `zfs_qat_dc_max_instances` and `zfs_qat_cy_max_instances`.
- Treat the value as a cap: the effective instance count is the smaller of the hardware-reported count and the configured cap.
- Reject invalid values and reject changes after the corresponding QAT path has initialized; dynamic resizing is not part of this work.
- Document that there is normally no reason to change these from `48` unless testing a driver, firmware, or platform-specific instance-selection issue.

Status: completed for the 2026-05-13 pass. The parameters were built, installed, reboot-tested, and validated on `pve.drewnet.online`.

Workstream F: optimization bias controls:

- The premise is useful but should not become a no-op API. Add bias parameters only when there are multiple proven policies to select between.
- A throughput/latency bias is valid if implementation choices create real tradeoffs, such as queue depth, batching, offload threshold, instance selection, or metadata reuse. Candidate values: `latency`, `balanced`, and `throughput`.
- A performance/compression-ratio bias is valid if implementation choices affect compression effort or fallback policy. Candidate values: `performance`, `balanced`, and `compressionratio`.
- Storage-media bias may also be needed. A higher-ratio policy can be more
  valuable on slower rotational pools than on flash/NVMe if reduced compressed
  bytes relieve the device bottleneck enough to offset QAT or CPU cost. Do not
  add `rotational` or `flash` profile inputs until HDD and NVMe results prove
  this is a repeatable policy split rather than benchmark noise. Initial paired
  HDD/NVMe testing on 2026-05-23 showed `1M` winning elapsed time on HDD at
  jobs `4` and `8` while losing on NVMe, so the premise is plausible but still
  needs confirmation with other source data and less-compressible input. A
  2026-05-24 random-data follow-up did not repeat the `1M` HDD win at higher
  concurrency when compression ratio stayed at `1.00x`, so storage-media bias
  should only be considered with explicit compression-ratio or byte-reduction
  evidence. Random incompressible data supports admission/fallback and
  compressibility-profile decisions, not a standalone rotational/flash default
  split. A 2026-05-24 mixed random/zero follow-up produced `1.96x` compression
  and still lost elapsed time on HDD at every tested `512K`/`1M` row, while
  winning on NVMe at jobs `4` and `8`. This makes expected compression ratio a
  first-class profile input; storage-media class should be a modifier rather
  than the primary selector.
- Circle back to media-bias profiles with a full sweep media benchmark. The
  sweep must assess every current QAT profile-driven setting and manual
  tunable individually across HDD and NVMe, not only the few settings already
  noted as possible media-bias candidates. The goal is to identify all settings
  whose best value changes by media class before adding `rotational`, `flash`,
  or similar profile inputs.
- Do not assume every QAT tuning knob cleanly maps to one bias. `zfs_qat_cpa_dc_level` directly affects QAT compression effort, but larger record eligibility, allocation reuse, and software fallback thresholds may affect throughput and latency without improving ratio.
- Initial implementation should keep explicit low-level parameters available for controlled benchmarking. Bias parameters can later set coherent defaults for those lower-level knobs once measurements prove the policies.

Status update: global tunables should participate in profiles rather than
remaining manual forever. Because a host can contain multiple pools or datasets
with different record sizes, the profile must use an operator-selected target
record size instead of inferring one automatically. The default target should be
`131072`, matching OpenZFS's default `128K` dataset recordsize. See
`phase-4-profile-plan.md`.

Profile implementation status:

- `zfs_qat_dc_profile=balanced`,
  `zfs_qat_dc_ratio_profile=balanced`, and
  `zfs_qat_dc_profile_recordsize=131072` are implemented as validated profile
  inputs.
- `zfs_qat_dc_expected_ratio=unknown|low|medium|high` is implemented as the
  first compressibility-profile input. `unknown` preserves the previous
  behavior. `low`, `medium`, and `high` only affect tunables that remain set to
  `profile`.
- Profile-owned tunables can default to `profile` while still accepting concrete
  manual values for per-tunable override.
- Effective settings for async cap policy, large-record eligibility,
  decompression policy, compression level, Huffman type, async enablement/retry
  values, and source/destination coalescing are computed from the active
  profile when the relevant tunable is set to `profile`.
- `zfs_qat_dc_expected_ratio=medium` caps profile-managed maximum request size
  at `512K` and keeps profile-managed async cap behavior at balanced
  `recordsize`, reflecting the moderate-compressibility benchmark where `1M`
  records did not justify their elapsed-time cost.
- `zfs_qat_dc_expected_ratio=low` with balanced ratio profile selects static
  Huffman and raises the profile-managed minimum request size to `512K`.
- `zfs_qat_dc_expected_ratio=high` preserves balanced compression effort. The
  first validation pass rejected automatically selecting QAT level 4 for this
  hint because it broadly regressed elapsed time.
- Apply session-global settings such as QAT compression level and Huffman type
  only before QAT DC initialization, and reject profile changes that would
  require changing active QAT DC sessions.
- Keep deployment/resource settings such as DC/CY split, max-instance caps,
  checksum disablement, and encryption disablement as host-profile recipe items
  rather than dynamically mutated profile state.

Acceptance:

- QAT gzip has a documented comparison against software gzip for throughput, p50/p95/p99 latency, CPU cost, compression ratio, and correctness.
- Large-record behavior is explicitly proven as either QAT-offloaded or software fallback for each tested record size.
- Any default change is justified by measurements on QAT 1.x hardware, not by assumptions from QAT 2.0+ or community reports.
- NUMA conclusions remain out of scope until testing on a true multi-socket host.

## Phase 5: Host Validation

Purpose: prove the implementation on actual QAT 1.x hardware.

Work:

- Use `test-hdd-pool` for controlled write/read tests.
- Use `nvme_scratch` only as read-only source data unless a test explicitly needs writable scratch space.
- Capture QAT kstats before and after each workload.
- Compare software gzip and QAT gzip datasets with equivalent data.
- Validate reads after module reload or reboot if practical.
- Preserve existing benchmark scripts under `/root/`, but review them before trusting their results.

Minimum host checks:

```bash
zfs version
dkms status
modinfo zfs | egrep '^(filename|version|depends|parm: zfs_qat)'
cat /sys/module/zfs/parameters/zfs_qat_compress_disable
cat /proc/spl/kstat/zfs/qat
adf_ctl status
zpool status -v test-hdd-pool
```

Acceptance:

- QAT kstats prove offload occurred for the tested workload.
- Pool status remains clean.
- Written data can be read back successfully.
- Test notes identify dataset compression setting, recordsize if changed, source data, write size, elapsed time, and QAT counters.

## Phase 6: Documentation And Operator Workflow

Purpose: make the result repeatable after kernel or DKMS changes.

Work:

- Document exact DKMS rebuild flow for QAT-enabled ZFS on Proxmox 9.
- Document how to re-baseline `/usr/src/zfs-2.4.99/` safely.
- Document required QAT service state and `/etc/dh895xcc_dev0.conf` or `/etc/c6xx_dev0.conf` expectations.
- Document runtime toggles, kstats, and expected failure modes.
- Update the skill references when source paths, host state, or validation commands change.

Acceptance:

- A future agent can rebuild, load, validate, and benchmark QAT-enabled ZFS without rediscovering the host setup.
- Documentation explicitly says not to install dracut packages on `pve.drewnet.online`.

## Phase 7: Secondary Crypto And Checksum Work

Purpose: evaluate SHA256 and AES-GCM only after compression is stable.

Work:

- Verify QAT crypto instances on dh895x/C620 with the active config.
- Prove SHA256 checksum kstats move under a controlled workload before changing policy.
- Prove AES-GCM offload only for supported buffer classes; keep current ZIL and dnode exclusions unless a safe implementation change is justified.
- Do not add QAT 2.0+ crypto features.

Acceptance:

- Any crypto/checksum change has separate correctness tests and kstat evidence.
- Compression improvements are not blocked by crypto/checksum scope.

## Immediate Next Steps

1. Run a focused profile sweep comparing `balanced`, `latency`, `throughput`,
   and `offload` at the configured target record sizes. Status: completed on
   2026-05-18; see `profile-sweep-20260518.md`.
2. Repeat the strongest candidate from the profile sweep:
   `target=1M`, `profile=throughput`, `record=1M`, first with more iterations
   at `JOBS=4`, then at `JOBS=8`.
3. Capture and evaluate QAT-side optimization state before adding more ZFS
   performance code: service split, polling mode, PCIe link state, NUMA
   placement, parameter-checking options, and alignment behavior. See
   `performance-optimization-next-steps.md`.
4. Promote only repeatable profile wins into the documented default mappings;
   keep weak, mixed, or software-fallback-only results as manual overrides.
5. Defer checksum and encryption policy changes until compression profile
   behavior is stable.
