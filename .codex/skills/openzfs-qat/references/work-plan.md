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

Status: completed for the 2026-05-12 pass. See `phase-4-results.md`.

Work:

- Re-evaluate the compression offload window for QAT 1.x and ZFS record sizes.
- Measure failed offload attempts and software fallback frequency.
- Review allocation and copy costs in `qat_compress_impl()`, especially buffer-list metadata and scratch buffers.
- Evaluate whether the fixed `QAT_DC_MAX_INSTANCES = 48` and `QAT_CRYPT_MAX_INSTANCES = 48` caps are harmless for dh895x/C620 or should be made dynamic.
- Account for NUMA placement on `pve.drewnet.online`; the QAT device is on NUMA node `2`.

Acceptance:

- Any threshold or allocation change is backed by before/after host measurements.
- Performance tests record CPU cost, throughput, compression ratio, QAT kstats, and failure counters.
- Tuning changes do not reduce correctness or fallback safety.

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

1. Document and, if needed, improve the boot ordering between `qat.service` and early ZFS module load.
2. Extend phase 5 host validation with repeatable benchmark scripts and read-after-reboot checks.
3. Evaluate per-request allocation and mapping reductions in `qat_compress_impl()` as a focused follow-up if QAT throughput remains important.
4. Defer checksum and encryption policy changes until compression behavior is stable.
