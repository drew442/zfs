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
19. For the QAT parameter-checking experiment, use `references/qat-param-check-experiment-20260523.md`.
20. For the QAT `[KERNEL_QAT]` DC polling-mode experiment, use `references/qat-kernelqat-polling-20260523.md`.
21. For the expanded QAT completion-mode matrix and lock-step config helper, use `references/qat-completion-mode-lockstep-20260523.md`.
22. For the imported host-root artifact corpus and chronological performance progression table, use `references/performance-progression-20260523.md`.
23. For the async page-array and scratch-buffer reuse optimization, use `references/qat-scratch-reuse-20260523.md`.
24. For the post-scratch-reuse full record-size matrix, use `references/qat-scratch-reuse-fullmatrix-20260523.md`.
25. For the offload-profile async cap policy and QAT/ZFS boot-order cleanup, use `references/qat-offload-profile-cap-20260523.md`.
26. For the profile-driven minimum QAT DC request-size policy, use `references/qat-minbuf-profile-policy-20260523.md`.
27. For the NVMe-backed validation of the min-buffer policy and benchmark run-order update, use `references/qat-minbuf-nvme-policy-20260523.md`.
28. For the paired `RUN_ORDER=record` NVMe min-buffer validation, use `references/qat-minbuf-nvme-recordorder-20260523.md`.
29. For the paired HDD media comparison of the min-buffer policy, use `references/qat-minbuf-hdd-recordorder-20260523.md`.
30. For the random-data HDD/NVMe min-buffer check, use `references/qat-minbuf-random-media-recordorder-20260524.md`.
31. For the compressibility/media matrix, use `references/qat-compressibility-media-matrix-20260524.md`.
32. For expected-ratio profile validation, use `references/qat-expected-ratio-validation-20260524.md`.
33. For exact page-count and scratch page-array request-overhead reduction, use `references/qat-page-count-overhead-20260524.md`.
34. For buffer-slot scratch page-array reuse, use `references/qat-slot-scratch-reuse-20260524.md`.
35. For buffer-slot source/destination page-array reuse, use `references/qat-slot-page-array-reuse-20260525.md`.
36. For buffer-slot sync request reuse, use `references/qat-slot-sync-req-reuse-20260525.md`.
37. For buffer-slot async request reuse, use `references/qat-async-req-slot-reuse-20260525.md`.
38. For async request prepare stack-clear reduction, use `references/qat-async-req-prepare-skip-stacks-20260525.md`.
39. For profile-gated detailed shape stats, use `references/qat-shape-stats-gating-20260525.md`.
40. For cached page address-space lookup during QAT buffer-list construction, use `references/qat-page-lookup-cache-20260525.md`.
41. For the rejected linear-buffer `kmap()` bypass experiment, use `references/qat-linear-kmap-bypass-20260526.md`.
42. For the rejected async cap precheck experiment, use `references/qat-async-precheck-20260527.md`.
43. For the rejected combined async completion/timeout status check experiment, use `references/qat-async-status-combined-20260527.md`.
44. For the rejected contiguous scratch flat-buffer experiment, use `references/qat-contiguous-scratch-20260527.md`.
45. For the current-code 1M async cap-policy matrix, use `references/qat-current-cap-matrix-20260527.md`.
46. For the current dual-media baseline and request-overhead interpretation, use `references/qat-current-dual-media-baseline-20260527.md`.
47. For compression eligibility/fallback reason counters and the first HDD/NVMe
    attribution run, use `references/qat-eligibility-counters-20260527.md`.
48. For the `512K` async cap sweep showing why the default profile should not
    raise `512K` caps to all DC instances, use `references/qat-cap512-sweep-20260527.md`.
49. For profile-gated async local timing stats and the HDD/NVMe `512K`/`1M`
    request-overhead validation, use `references/qat-async-timing-stats-gating-20260527.md`.
50. For profile-gated sync compression timing stats and the HDD/NVMe default
    plus throughput/1M validation, use `references/qat-sync-timing-stats-gating-20260527.md`.
51. For profile-gated aggregate request-shape counters, use
    `references/qat-aggregate-shape-stats-gating-20260527.md`.
52. For profile-gated request-path diagnostics such as buffer-slot reuse,
    page-array path, and request-slot reuse counters, use
    `references/qat-request-path-stats-gating-20260528.md`.
53. Confirm which layer the task touches before editing:
   - Build integration and static configuration
   - Runtime tunables and initialization
   - Compression, checksum, or encryption call sites
   - Documentation or validation workflow
54. Preserve software fallback behavior unless the task explicitly changes policy. Current QAT paths generally attempt acceleration first and then fall back to software on failure.
55. Treat usability and performance separately:
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
