---
name: openzfs-qat
description: Use for OpenZFS QAT-specific or QAT-adjacent design, implementation, validation, performance, memory/allocation, worker-path, compression/checksum/encryption, host-operation, or benchmark tasks in this repository. Do not use for genuinely unrelated OpenZFS work, generic C cleanup, formatting-only changes, docs-only edits outside QAT, or non-QAT build/test questions unless the task could affect QAT behavior, performance interpretation, host safety, or project continuity.
---

# OpenZFS QAT

Use this skill for QAT-specific and QAT-adjacent work in this fork. It is meant for design review, implementation changes, host validation, benchmarking, and preservation of QAT project continuity rather than unrelated ZFS work.

## First-Class Requirement: Expert Continuity

Preserve the current high-context ZFS/QAT specialist behavior. The purpose of this skill is not to minimize context at all costs; it is to keep project-specific judgment, design intent, host-specific operational safety, and benchmark interpretation consistent across sessions.

Prefer spending context on stable, decision-preserving project memory when it materially reduces rediscovery, unsafe host actions, shallow conclusions, or repeated failed experiments.

Treat the following as high-value continuity context:

- QAT 1.x scope and explicit exclusion of QAT 2.0+, Gen4-only, SVM-only, QATlib-only, and in-tree-only paths unless they also apply to this project.
- Preserve software fallback and safe failure behavior unless the task explicitly changes policy.
- `pve.drewnet.online` safety rules, especially that it is an approved test host but must not have dracut packages installed.
- Prior benchmark conclusions, disproven optimization shapes, and host-specific caveats when doing performance, validation, or QAT-adjacent design work.
- The distinction between usability, performance, correctness, and operator workflow work.

Optimize token use by loading context intentionally, not by discarding expert memory.

## Project Scope

- Focus support on QAT 1.x cards, specifically dh895x/dh895xcc and C620/C62x-class hardware.
- Do not pursue features, APIs, driver layouts, or tuning paths that are only available on QAT hardware version 2.0 or newer.
- Use QAT 2.0+ documentation only as background when the behavior also applies to QAT 1.x.

## Scope Boundary

Classify narrowly only when the task is clearly unrelated to QAT. When in doubt, treat the task as QAT-adjacent and preserve expert continuity.

### QAT-specific work

Use the full QAT expert context when the task directly touches:

- QAT build detection, `ICP_ROOT`, DKMS, module dependencies, or external Intel driver assumptions.
- QAT module parameters, kstats, initialization, lazy re-enable, or fallback behavior.
- QAT compression, decompression, checksum, AES-GCM, thresholds, instance selection, buffer lists, coalescing, or allocation paths.
- QAT documentation, benchmark scripts, phase results, or operator workflow.
- `pve.drewnet.online` validation or sysadmin work related to this project.

### QAT-adjacent OpenZFS work

Also use this skill when generic-looking OpenZFS work could change, explain, or be explained by QAT behavior or prior QAT conclusions. This includes forming expert opinions or making changes around:

- ZIO worker scheduling, taskqs, queueing, sync-vs-async behavior, concurrency, latency, or throughput paths used by compression/checksum/encryption.
- Memory allocation, buffer lifetimes, page mapping, scatter/gather handling, kmem/vmem usage, ABD layout, ARC interactions, or temporary scratch-buffer policy.
- Compression pipeline behavior, gzip level semantics, recordsize thresholds, readback/decompression behavior, checksum interaction, or encrypted I/O paths.
- CPU cost, NUMA locality, offload-vs-software tradeoffs, benchmark methodology, or interpretation of host measurements.
- Any optimization or cleanup that might change the cost model for QAT offload, software fallback, or measured performance.

For QAT-adjacent work, load at least `references/repo-map.md`, then load performance, host, or source notes if the decision depends on prior measurements, disproven optimization shapes, or host caveats.

### Genuinely unrelated generic ZFS work

Only treat work as generic/unrelated when it does not touch QAT behavior, QAT-adjacent performance paths, host validation, or project continuity. Examples may include:

- Formatting-only changes outside QAT files.
- Documentation edits unrelated to QAT, compression, checksum, encryption, performance, or host validation.
- Isolated tests or code paths with no relationship to compression/checksum/encryption, ZIO scheduling, memory allocation, module loading, DKMS, or the benchmarked host.
- Repository maintenance unrelated to this QAT branch's build, validation, or benchmark workflow.

If a task could reasonably affect QAT conclusions or require prior benchmark context, do not classify it as generic.

## Context Loading Policy

Always load enough context to preserve expert continuity and avoid repeating prior mistakes. Do not load every reference document by default.

### Default lightweight path

For small or local QAT edits and QAT-adjacent triage, start with `references/repo-map.md`. This is the compact expert map and should usually be enough to identify the relevant files, module parameters, constraints, and commands.

Examples:

- Local code review around an existing QAT symbol or parameter.
- Small documentation update for a known QAT knob.
- Focused build, macro, or man-page edit where no host testing or benchmark interpretation is needed.
- Initial review of memory, worker, or allocation questions that may affect QAT but do not yet require historical benchmark detail.

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

For benchmark design, benchmark execution, performance interpretation, threshold decisions, memory allocation strategy, worker/concurrency changes, allocation/copy strategy, instance selection, or compression-level policy, read:

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
- Preserve long-form benchmark/history documents, but consult them whenever they are decision-relevant.
- When QAT-adjacent work depends on prior performance conclusions, load the relevant performance/history context rather than guessing from generic OpenZFS knowledge.
- When adding new project memory, prefer compact, decision-preserving summaries over duplicating raw logs or CSVs.
- Do not paste long benchmark CSVs, full command transcripts, or large diffs into discussion unless needed for the task.
- Never reduce context so far that the agent loses project continuity, host safety awareness, or prior benchmark conclusions.

## Workflow

1. Classify the task before loading references:
   - QAT-specific work
   - QAT-adjacent OpenZFS work
   - Genuinely unrelated generic ZFS work
   - Build or driver integration
   - Host validation or sysadmin work
   - Performance/benchmark work
   - Project sequencing or documentation
2. When classification is ambiguous, choose QAT-adjacent rather than generic.
3. Load the smallest reference set that preserves expert continuity for that task, following the Context Loading Policy.
4. Confirm which layer the task touches before editing:
   - Build integration and static configuration
   - Runtime tunables and initialization
   - Compression, checksum, or encryption call sites
   - QAT-adjacent worker, memory, scheduling, or allocation paths
   - Documentation or validation workflow
5. Preserve software fallback behavior unless the task explicitly changes policy. Current QAT paths generally attempt acceleration first and then fall back to software on failure.
6. Treat usability and performance separately:
   - Usability work usually means clearer build flags, safer defaults, less surprising runtime behavior, or better observability.
   - Performance work usually means thresholding, allocation strategy, instance selection, avoiding unnecessary copies, or reducing failed offload attempts.
7. For phase 4 benchmark runs, prefer `scripts/qat-phase4-benchmark.sh` so throughput, latency summaries, CPU cost, QAT kstats, module settings, and correctness checks are captured consistently.

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
- For QAT-adjacent worker, memory, allocation, compression, checksum, encryption, or ZIO changes, inspect both the direct code path and the QAT benchmark/performance implications before concluding the change is safe.
- For external driver assumptions, verify against current Intel docs and the actual `ICP_ROOT` tree being built against.
- Do not install dracut packages on `pve.drewnet.online`; it boots with initramfs and dracut package changes are explicitly out of scope.
- When QAT hardware or drivers are unavailable, validate that the software fallback still builds cleanly and the non-QAT path remains unchanged.
- If more detail is needed, read `references/repo-map.md` first, then load only the reference files relevant to the task class.
