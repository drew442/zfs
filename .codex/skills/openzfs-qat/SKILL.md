---
name: openzfs-qat
description: Use when reviewing or changing OpenZFS QAT support in this repository, especially build detection, static configuration, module parameters, fallback behavior, performance thresholds, or Linux implementation details for compression, checksum, and AES-GCM paths.
---

# OpenZFS QAT

Use this skill for QAT-specific work in this fork. It is meant for design review, implementation changes, and validation of usability or performance behavior rather than generic ZFS work.

## Workflow

1. Start with the repo map in `references/repo-map.md`.
2. Confirm which layer the task touches before editing:
   - Build integration and static configuration
   - Runtime tunables and initialization
   - Compression, checksum, or encryption call sites
   - Documentation or validation workflow
3. Preserve software fallback behavior unless the task explicitly changes policy. Current QAT paths generally attempt acceleration first and then fall back to software on failure.
4. Treat usability and performance separately:
   - Usability work usually means clearer build flags, safer defaults, less surprising runtime behavior, or better observability.
   - Performance work usually means thresholding, allocation strategy, instance selection, avoiding unnecessary copies, or reducing failed offload attempts.

## Review Focus

- Check whether the change depends on compile-time assumptions such as fixed instance counts, fixed buffer thresholds, or specific external QAT driver layouts.
- Check whether runtime enablement is discoverable and reversible through module parameters.
- Check whether a failure path silently disables acceleration for too much of the system.
- Check whether the code copies or allocates more memory than the offload saves.
- Check whether documentation still matches the actual knobs and code paths.

## Validation

- Use `rg -n "QAT|qat_"` to expand the impact surface before changing behavior.
- For build-facing changes, inspect `config/kernel.m4`, `config/zfs-build.m4`, and `scripts/dkms.mkconf`.
- For runtime behavior, inspect the module parameters and kstat updates first.
- When QAT hardware or drivers are unavailable, validate that the software fallback still builds cleanly and the non-QAT path remains unchanged.
- If you need more detail, read `references/repo-map.md`.
