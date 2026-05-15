---
name: openzfs-qat
description: Use for OpenZFS QAT-specific design, implementation, validation, or benchmark tasks in this repository. Do not use for generic OpenZFS work, generic C cleanup, formatting-only changes, docs-only edits outside QAT, or non-QAT build/test questions unless the task touches QAT behavior or project continuity.
---

# OpenZFS QAT

Use this skill for QAT-specific work in this fork. It is meant for design review, implementation changes, host validation, benchmarking, and preservation of QAT project continuity rather than generic ZFS work.

## First-Class Requirement: Expert Continuity

Preserve the current high-context ZFS/QAT specialist behavior. The purpose of this skill is not to minimize context at all costs; it is to keep project-specific judgment, design intent, host-specific operational safety, and benchmark interpretation consistent across sessions.

Prefer spending context on stable, decision-preserving project memory when it materially reduces rediscovery, unsafe host actions, shallow conclusions, or repeated failed experiments.

Treat the following as high-value continuity context:

- QAT 1.x scope and explicit exclusion of QAT 2.0+, Gen4-only, SVM-only, QATlib-only, and in-tree-only paths unless they also apply to this project.
- Preserve software fallback and safe failure behavior unless the task explicitly changes policy.
- `pve.drewnet.online` safety rules, especially that it is an approved test host but must not have dracut packages installed.
- Prior benchmark conclusions, disproven optimization shapes, and host-specific caveats when doing performance or validation work.
- The distinction between usability, performance, correctness, and operator workflow work.

Optimize token use by loading context intentionally, not by discarding expert memory.

## Project Scope

- Focus support on QAT 1.x cards, specifically dh895x/dh895xcc and C620/C62x-class hardware.
- Do not pursue features, APIs, driver layouts, or tuning paths that are only available on QAT hardware version 2.0 or newer.
- Use QAT 2.0+ documentation only as background when the behavior also applies to QAT 1.x.

## Context Loading Policy

Always load enough context to preserve expert continuity and avoid repeating prior mistakes. Do not load every reference document by default.

### Default lightweight path

For small or local QAT edits, start with `references/repo-map.md`. This is the compact expert map and should usually be enough to identify the relevant files, module parameters, constraints, and commands.

Examples:

- Local code review around an existing QAT symbol or parameter.
- Small documentation update for a known QAT knob.
- Focused build, macro, or man-page edit where no host testing or benchmark interpretation is needed.

### Build and driver path

For build integration, configure, DKMS, external driver, or `ICP_ROOT` questions, read:

1. `references/repo-map.md`
2. The build/driver-relevant sections of `references/source-notes.md`

Read more of `source-notes.md` only when the task depends on Intel API behavior, QAT driver assumptions, or a source-backed claim.

### Host validation path

Before making changes on, or running commands against, `pve.drewnet.online`, read:

1. `references/repo-map.md`
2. `references/pve-drewnet-online.md`

Host safety and boot-order context are more important than token reduction for host work.

### Performance and benchmark path

For benchmark design, benchmark execution, performance interpretation, threshold decisions, allocation/copy strategy, instance selection, or compression-level policy, read:

1. `references/repo-map.md`
2. `references/phase-4-performance-review.md`
3. Relevant sections of `references/phase-4-results.md` only when detailed historical evidence is needed
4. `references/pve-drewnet-online.md` before touching the host

Performance conclusions must remain continuity-preserving and measurement-backed. Do not repeat previously disproven optimization shapes unless the task explicitly asks to revisit them with new evidence.

### Project sequencing path

For roadmap, phase planning, or deciding what work should come next, read:

1. `references/repo-map.md`
2. `references/work-plan.md`
3. Phase result files only when the question depends on completed acceptance criteria or detailed prior outcomes

### Completed phase result files

Use `references/phase-0-1-results.md`, `references/phase-2-3-results.md`, `references/phase-4-results.md`, and `references/phase-4-performance-review.md` as detailed evidence, not as mandatory startup context for every task.

## Token Discipline

- Prefer targeted inspection over loading full reference files.
- Do not read all reference documents by default.
- Use `rg`, narrow file ranges, and specific section reads before opening large files in full.
- Preserve long-form benchmark/history documents, but consult them only when they are decision-relevant.
- When adding new project memory, prefer compact, decision-preserving summaries over duplicating raw logs or CSVs.
- Do not paste long benchmark CSVs, full command transcripts, or large diffs into discussion unless needed for the task.
- Never reduce context so far that the agent loses project continuity, host safety awareness, or prior benchmark conclusions.

## Workflow

1. Classify the task before loading references:
   - Small/local QAT edit
   - Build or driver integration
   - Host validation or sysadmin work
   - Performance/benchmark work
   - Project sequencing or documentation
2. Load the smallest reference set that preserves expert continuity for that task, following the Context Loading Policy.
3. Confirm which layer the task touches before editing:
   - Build integration and static configuration
   - Runtime tunables and initialization
   - Compression, checksum, or encryption call sites
   - Documentation or validation workflow
4. Preserve software fallback behavior unless the task explicitly changes policy. Current QAT paths generally attempt acceleration first and then fall back to software on failure.
5. Treat usability and performance separately:
   - Usability work usually means clearer build flags, safer defaults, less surprising runtime behavior, or better observability.
   - Performance work usually means thresholding, allocation strategy, instance selection, avoiding unnecessary copies, or reducing failed offload attempts.
6. For phase 4 benchmark runs, prefer `scripts/qat-phase4-benchmark.sh` so throughput, latency summaries, CPU cost, QAT kstats, module settings, and correctness checks are captured consistently.

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
- If more detail is needed, read `references/repo-map.md` first, then load only the reference files relevant to the task class.
