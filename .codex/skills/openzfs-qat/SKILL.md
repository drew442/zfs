---
name: openzfs-qat
description: Use when reviewing or changing OpenZFS QAT support in this repository, especially build detection, static configuration, module parameters, fallback behavior, performance thresholds, or Linux implementation details for compression, checksum, and AES-GCM paths.
---

# OpenZFS QAT

Use this skill for QAT-specific work in this fork. It is meant for design review, implementation changes, and validation of usability or performance behavior rather than generic ZFS work.

## Project Scope

- Focus support on QAT 1.x cards, specifically dh895x/dh895xcc and C620/C62x-class hardware.
- Do not pursue features, APIs, driver layouts, or tuning paths that are only available on QAT hardware version 2.0 or newer.
- Use QAT 2.0+ documentation only as background when the behavior also applies to QAT 1.x.

## Workflow

1. Start with the repo map in `references/repo-map.md`.
2. For driver/API questions, check `references/source-notes.md` before relying on community posts or old comments.
3. For host testing on `pve.drewnet.online`, read `references/pve-drewnet-online.md` before making changes or running benchmarks.
4. For project sequencing, use `references/work-plan.md`.
5. For QAT driver packaging and ZFS/QAT DKMS lock-step builds, use `references/qat-dkms-lockstep.md`.
6. For completed implementation passes, check `references/phase-0-1-results.md`, `references/phase-2-3-results.md`, and `references/phase-4-results.md`.
7. For human-review phase 4 performance charts and summary tables, use `references/phase-4-performance-review.md`.
8. For benchmark interpretation, use `references/benchmark-evaluation-methodology.md` so hybrid fallback wins are not confused with QAT engine improvements.
9. For phase 4 benchmark runs, prefer `scripts/qat-phase4-benchmark.sh` so throughput, latency summaries, CPU cost, QAT kstats, module settings, derived offload-share metrics, and correctness checks are captured consistently.
10. For QAT driver-side timing results, use `references/qat-driver-timing-matrix-20260519.md`.
11. For the interrupt coalescing minimum-timer experiment, use `references/qat-coalescing-min-timer-20260519.md`.
12. For the QAT DC polling implementation and results, use `references/qat-dc-polling-20260519.md`.
13. For the completed async/profile matrix and request-overhead step 5 plan, use `references/async-profile-matrix-20260521.md`.
14. For the first request-overhead baseline and next optimization target, use `references/request-overhead-baseline-20260522.md`.
15. For the source-coalescing request-shape retest, use `references/source-coalescing-request-shape-20260522.md`.
16. For alignment/segment-shape instrumentation, use `references/alignment-shape-instrumentation-20260522.md`.
17. For the QAT platform/service baseline and service-split results, use `references/qat-platform-service-baseline-20260522.md`.
18. For the QAT/ZFS DKMS initramfs recovery procedure, use `references/qat-dkms-initramfs-recovery-20260523.md`.
19. Confirm which layer the task touches before editing:
   - Build integration and static configuration
   - Runtime tunables and initialization
   - Compression, checksum, or encryption call sites
   - Documentation or validation workflow
18. Preserve software fallback behavior unless the task explicitly changes policy. Current QAT paths generally attempt acceleration first and then fall back to software on failure.
19. Treat usability and performance separately:
   - Usability work usually means clearer build flags, safer defaults, less surprising runtime behavior, or better observability.
   - Performance work usually means thresholding, allocation strategy, instance selection, avoiding unnecessary copies, or reducing failed offload attempts.

## Review Focus

- Prefer Intel documentation, OpenZFS code, and first-hand implementation artifacts over forum summaries. Use community links as leads, not as authority.
- Check whether the change depends on compile-time assumptions such as fixed instance counts, fixed buffer thresholds, or specific external QAT driver layouts.
- Check whether runtime enablement is discoverable and reversible through module parameters.
- Check whether a failure path silently disables acceleration for too much of the system.
- Check whether the code copies or allocates more memory than the offload saves.
- Check whether documentation still matches the actual knobs and code paths.
- Check whether a claim applies to QAT 1.x dh895x/C620 hardware before using it. Exclude QAT 2.0+, Gen4-only, and in-tree-only features from project changes.

## Validation

- Use `rg -n "QAT|qat_"` to expand the impact surface before changing behavior.
- For build-facing changes, inspect `config/kernel.m4`, `config/zfs-build.m4`, and `scripts/dkms.mkconf`.
- For runtime behavior, inspect the module parameters and kstat updates first.
- For external driver assumptions, verify against current Intel docs and the actual `ICP_ROOT` tree being built against.
- Do not install dracut packages on `pve.drewnet.online`; it boots with initramfs and dracut package changes are explicitly out of scope.
- When QAT hardware or drivers are unavailable, validate that the software fallback still builds cleanly and the non-QAT path remains unchanged.
- If you need more detail, read `references/repo-map.md`.
