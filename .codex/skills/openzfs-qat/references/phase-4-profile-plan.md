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
zfs_qat_dc_profile=balanced|latency|throughput|offload
zfs_qat_dc_profile_recordsize=131072|262144|524288|1048576
zfs_qat_dc_ratio_profile=balanced|performance|ratio
```

Semantics:

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
- `zfs_qat_dc_profile_recordsize=131072` is the default because OpenZFS
  defaults the dataset `recordsize` property to `128K`
  (`SPA_OLD_MAXBLOCKSIZE`).

The record-size parameter is deliberately explicit. It should not be inferred
from whichever dataset happens to initialize QAT first.

## Managed Tunables

Tunables that are included in profiles should default to `profile`. Setting a
managed tunable to `profile` means the active profile computes the effective
value. Setting it to a concrete value makes that one tunable manual while the
rest of the profile remains active.

Example:

```text
zfs_qat_dc_profile=throughput
zfs_qat_dc_profile_recordsize=1048576
zfs_qat_dc_ratio_profile=balanced
zfs_qat_dc_async=profile
zfs_qat_dc_async_cap_policy=profile
zfs_qat_decompress_disable=0
```

In that example the profile controls async enablement and cap policy, but the
operator explicitly overrides decompression policy.

If an operator wants to return an overridden tunable to profile control, they
write `profile` back to that tunable.

## Precedence

Use this order:

1. For profile-managed tunables set to `profile`, compute the value from
   `zfs_qat_dc_profile`, `zfs_qat_dc_profile_recordsize`, and
   `zfs_qat_dc_ratio_profile`.
2. For profile-managed tunables set to a concrete value, use the concrete
   manual value for that tunable only.
3. For deployment/manual tunables, always use the configured value.

Low-level knobs remain available for benchmarking and debugging. The important
change is that manual override is per tunable, not an all-or-nothing global
mode.

Implementation note: existing integer module parameters cannot keep using the
plain integer setter if they need to accept the string `profile`. Each managed
numeric tunable needs a custom setter/getter that accepts either `profile` or a
validated concrete value, stores whether the tunable is profile-managed, and
reports `profile` when profile control is active.

## Knob Classification

```text
knob                                  scope          profile handling
zfs_qat_dc_async                     global         profile|0|1
zfs_qat_dc_async_cap_policy          per request    profile|fixed|recordsize|throughput
zfs_qat_dc_async_max_inflight        global cap     profile|integer
zfs_qat_dc_async_submit_retries      global         profile|integer
zfs_qat_dc_async_retry_us            global         profile|integer
zfs_qat_decompress_disable           global         profile|0|1
zfs_qat_dc_max_buf_size              global         profile|131072|262144|524288|1048576
zfs_qat_dc_coalesce_src              global path    profile|0|1 after repeat evidence
zfs_qat_dc_coalesce_dst              global path    profile|0|1 after repeat evidence
zfs_qat_cpa_dc_level                 session-global profile|1|2|3|4 before DC init
zfs_qat_cpa_dc_hufftype              session-global profile|dynamic|static before DC init
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

1. Add `zfs_qat_dc_profile_recordsize` with default `131072` and validation for
   `128K`, `256K`, `512K`, and `1M`. Initial implementation accepts numeric
   byte values.
2. Add `zfs_qat_dc_profile=balanced` and
   `zfs_qat_dc_ratio_profile=balanced` as validated string parameters.
3. Convert the first profile-owned tunables to accept `profile` plus concrete
   manual values. Start with async cap policy because it has measured profile
   actions and does not by itself enable async QAT.
4. Implement effective-profile helper functions. The raw module parameter value
   describes operator intent; helper functions provide the effective runtime
   value.
5. Move the existing `recordsize` and `throughput` cap behavior behind the
   profile helpers while preserving concrete low-level manual values.
6. Apply session-global compression level and Huffman selections at QAT DC init
   only. Reject concrete/profile changes that would require changing active QAT
   DC sessions after initialization.
7. Add kstats or benchmark columns showing selected profiles, target record
   size, raw tunable state, and effective runtime choices.
8. Only then consider profile-managed coalescing or ratio/performance profiles.

Initial implementation status:

- `zfs_qat_dc_profile`, `zfs_qat_dc_profile_recordsize`, and
  `zfs_qat_dc_ratio_profile` exist as validated module parameters.
- `zfs_qat_dc_async_cap_policy` accepts `profile`, `fixed`, `recordsize`, and
  `throughput`, and defaults to `profile`.
- `zfs_qat_dc_max_buf_size` accepts `profile` or a concrete size and defaults
  to `profile`; its effective value is the profile target record size unless
  the operator supplies a concrete manual override.
- `zfs_qat_cpa_dc_level` accepts `profile` or a concrete level and defaults to
  `profile`; its effective value is level `1` for `balanced` and
  `performance` ratio profiles, and level `4` for the `ratio` profile.
- When `zfs_qat_dc_async_cap_policy=profile`, the effective cap behavior is
  computed from `zfs_qat_dc_profile` and `zfs_qat_dc_profile_recordsize`.
- Current profile action is intentionally narrow:
  `throughput` or `offload` with target record size `1M` uses the measured
  `throughput` cap behavior; all other profile combinations use balanced
  `recordsize` behavior.
- `zfs_qat_dc_async` remains an integer and remains disabled by default. This
  profile slice does not automatically enable async QAT.

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
- Which concrete settings should be represented as strings versus numeric
  values in `/sys/module/zfs/parameters/` after adding `profile` support.
- Whether profile-managed compression level should default to level 1 for
  throughput/performance profiles or keep the current operator-selected level
  until a broader repeat confirms the ratio tradeoff.
- Whether profile state should be exported only through module parameters or
  also through QAT kstats for easier benchmark interpretation.
