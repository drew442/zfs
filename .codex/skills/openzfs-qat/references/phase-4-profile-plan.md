# Phase 4 QAT Profile Plan

Date: 2026-05-18.

This note defines how QAT profile tuning should work when some useful knobs are
global or session-global, but the workload decision is still partly driven by
ZFS record size.

## Problem

A host can have more than one pool or dataset, and those datasets can use
different record sizes. Several QAT tuning knobs are global:

- QAT DC session settings such as compression level and Huffman type.
- QAT maximum buffer size.
- Source and destination coalescing toggles.
- Async enablement and retry settings.
- Decompression policy.
- DC/CY instance allocation and max-instance caps.

Leaving these knobs manual forever is not acceptable. Global should mean one
selected host policy at a time, not no policy.

## Principle

Profiles should be driven by an operator-supplied representative record size.
The operator chooses the record size that matters most for the host, pool set, or
workload class, and the profile maps that target to coherent global and
per-request QAT settings.

This avoids pretending that the kernel can infer one correct answer when a host
has mixed datasets. It also keeps the policy general for one, two, or more QAT
cards because active DC instance count remains an input to the profile.

## Proposed Interface

Initial profile parameters:

```text
zfs_qat_dc_profile=manual|balanced|latency|throughput|offload
zfs_qat_dc_profile_recordsize=0|131072|262144|524288|1048576
zfs_qat_dc_ratio_profile=balanced|performance|ratio
```

Semantics:

- `manual`: existing low-level parameters are authoritative.
- `balanced`: safe default profile. Preserve software fallback, avoid measured
  regressions, and use conservative record-size caps.
- `latency`: only offload record sizes that repeatedly beat same-window
  software or are neutral with a material CPU win.
- `throughput`: allow measured higher caps or path options when elapsed time or
  throughput improves for the target record size.
- `offload`: prefer lower CPU and higher QAT byte share when correctness and
  ratio are acceptable, even if elapsed time is only neutral.
- `performance`: ratio profile that may choose lower QAT compression effort or
  static Huffman only when the operator accepts ratio tradeoff.
- `ratio`: ratio profile that prefers dynamic Huffman and higher compression
  effort, accepting latency only when explicitly selected.
- `zfs_qat_dc_profile_recordsize=0` means no target has been selected. The
  profile should behave as `balanced` and should not apply target-specific
  session-global choices.

The record-size parameter is deliberately explicit. It should not be inferred
from whichever dataset happens to initialize QAT first.

## Precedence

Use this order:

1. `manual` profile: existing low-level module parameters are used exactly as
   set.
2. Non-manual profile with target record size: profile computes effective
   values for profile-managed knobs.
3. Non-manual profile without target record size: profile falls back to
   balanced-safe behavior and records that no target was selected.

Low-level knobs should remain available for benchmarking and debugging. Once a
non-manual profile owns a knob, documentation must say whether direct writes to
that low-level knob override the profile, are rejected, or are ignored until the
profile returns to `manual`. Prefer rejecting conflicting writes over silently
ignoring them.

## Knob Classification

```text
knob                                  scope          profile handling
zfs_qat_dc_async                     global         profile-managed
zfs_qat_dc_async_cap_policy          per request    profile-managed
zfs_qat_dc_async_max_inflight        global cap     manual/fixed fallback
zfs_qat_dc_async_submit_retries      global         profile-managed later
zfs_qat_dc_async_retry_us            global         profile-managed later
zfs_qat_decompress_disable           global         profile-managed
zfs_qat_dc_max_buf_size              global         profile-managed
zfs_qat_dc_coalesce_src              global path    profile-managed after repeat evidence
zfs_qat_dc_coalesce_dst              global path    profile-managed after repeat evidence
zfs_qat_cpa_dc_level                 session-global profile-managed before DC init
zfs_qat_cpa_dc_hufftype              session-global profile-managed before DC init
zfs_qat_dc_max_instances             init-global    deployment/manual
zfs_qat_cy_max_instances             init-global    deployment/manual
zfs_qat_checksum_disable             global         deployment/manual
zfs_qat_encrypt_disable              global         deployment/manual
QAT DC/CY service split              driver config  deployment/manual
```

Deployment/manual means the setting can be documented as part of a host profile
recipe, but should not be mutated dynamically by a ZFS profile.

## Initial Profile Mapping

Current evidence supports only a small first step:

```text
target recordsize profile     cap policy  max buf   coalesce  level/huff
128K              balanced    recordsize  128K+     off       unchanged
256K              balanced    recordsize  256K+     off       unchanged
1M                balanced    recordsize  1M        off       unchanged
1M                throughput  throughput  1M        off       unchanged
1M                offload     throughput  1M        off       unchanged
```

Notes:

- `throughput` cap policy currently only changes `1M+` caps. It keeps `128K`,
  `256K`, and untested `512K` at balanced caps.
- Coalescing is technically profile-eligible, but current repeat data does not
  justify enabling it automatically.
- Compression level and Huffman type are profile-eligible, but require a
  profile-aware initialization path because they are QAT DC session settings.

## Implementation Order

1. Add `zfs_qat_dc_profile_recordsize` with validation for `0`, `128K`, `256K`,
   `512K`, and `1M`.
2. Add `zfs_qat_dc_profile` and `zfs_qat_dc_ratio_profile` as validated string
   parameters.
3. Implement effective-profile helper functions instead of immediately mutating
   the existing low-level tunables.
4. Move the existing `recordsize` and `throughput` cap behavior behind the
   profile helpers while preserving the existing low-level cap-policy parameter
   for `manual`.
5. Apply session-global compression level and Huffman selections at QAT DC init
   only. Reject profile changes that would require changing active QAT DC
   sessions after initialization.
6. Add kstats or benchmark columns showing the selected profile, target record
   size, effective cap policy, and effective global choices.
7. Only then consider profile-managed coalescing or ratio/performance profiles.

## Mixed Dataset Guidance

When a host has mixed record sizes, choose the target based on the workload that
should receive the best QAT behavior:

- Choose `128K` only if smaller sync-style or latency-sensitive writes dominate.
- Choose `256K` if that is the largest common record size and larger-record QAT
  is not the priority.
- Choose `1M` for large-file, backup, imaging, media, VM image, or archival
  workloads where throughput and CPU offload matter most.

Datasets outside the target are still protected by the per-request admission and
fallback rules. The target does not force every record size through QAT.

## Open Questions

- Whether `512K` should inherit `1M` behavior or keep balanced caps until a
  dedicated benchmark exists.
- Whether `offload` and `throughput` should diverge for `1M+` after more CPU
  and elapsed-time evidence.
- Whether profile-managed compression level should default to level 1 for
  throughput/performance profiles or keep the current operator-selected level
  until a broader repeat confirms the ratio tradeoff.
- Whether profile state should be exported only through module parameters or
  also through QAT kstats for easier benchmark interpretation.
