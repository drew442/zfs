# QAT Benchmark Evaluation Methodology

Last updated: 2026-05-18.

This note defines how to interpret OpenZFS QAT benchmark results now that the
async compression path can intentionally fall back to software gzip when QAT
resources are exhausted or policy rules skip offload.

## Core Problem

`qat_ms` is not a pure QAT-performance metric when software fallback is enabled.
If QAT share falls and elapsed time improves, the result may mean the policy
avoided slow or saturated QAT work. That can be a valid user-facing result, but
it is not evidence that the QAT hardware path became faster.

Fallback remains a first-class feature. It protects users from hard latency
regressions, submit failures, and resource exhaustion. The benchmark method
therefore needs to evaluate two different questions separately:

- Should users enable this hybrid QAT policy?
- Did the QAT hardware path itself improve?

## Hybrid Policy Scorecard

Use this scorecard when evaluating whether a profile or admission policy is
worth enabling.

Storage-media context matters when interpreting elapsed time. A compression
policy with a higher compression ratio may help slower or more I/O-bound media
such as HDD pools more than faster flash or NVMe pools, because fewer compressed
bytes pass through the storage bottleneck. Treat this as a profile follow-up:
future HDD-vs-flash comparisons should record whether a result is dominated by
QAT service cost, CPU cost, or device write bandwidth before adding any
rotational/flash bias flag.

Required metrics:

- Elapsed time and throughput versus same-window software gzip.
- Run latency distribution: average, p50, p95, p99, and max.
- Compression ratio versus same-window software gzip.
- CPU active seconds per GiB and system CPU seconds per GiB.
- QAT byte share.
- QAT completion share.
- Fallback share and cap-skip share.
- Correctness: `sha_ok=yes`.
- Failure counters: `dc_fails`, async submit failures, retry failures, resource
  failures, and other failures.

Interpretation:

- A policy can be useful even when QAT share is low if latency, throughput,
  CPU cost, correctness, and ratio are acceptable.
- A low-share policy win must be described as a hybrid-policy win, not a QAT
  engine improvement.
- CPU savings are first-class. A policy that is neutral on latency but
  materially lowers CPU seconds per GiB can still be valuable.
- Compression ratio remains first-class. A performance win that materially
  reduces ratio must be labelled as a performance-vs-ratio tradeoff.

## QAT Engine Scorecard

Use this scorecard when evaluating whether a code change improved QAT itself.

Required metrics:

- QAT input bytes completed.
- QAT byte share, preferably `comp_in_delta / source_bytes`.
- QAT completion share, `dc_compress_async_completions_delta /
  dc_compress_async_submits_delta`.
- QAT setup, submit, wait, cleanup, and combined service nanoseconds per MiB.
- Submit failures, retry failures, resource failures, and other failures.
- Buffer-list shape and coalescing success/failure rates when relevant.
- Compression ratio for the same input and QAT compression level.

Interpretation:

- Do not claim a QAT engine improvement if elapsed time improved mostly because
  QAT share decreased.
- A QAT engine improvement should hold QAT byte share stable or increase it,
  unless the change explicitly targets admission policy rather than QAT path
  efficiency.
- Timing counters are service-cost indicators, not direct wall-clock throughput,
  because async requests can overlap.

## Outcome Labels

Use these labels in future phase 4 summaries:

- `qat-improvement`: elapsed time or CPU cost improves while QAT byte share is
  stable or higher, correctness holds, failures do not rise, and compression
  ratio is acceptable.
- `hybrid-policy-win`: elapsed time, throughput, or CPU cost improves because
  fallback avoids slow or saturated QAT work.
- `cpu-offload-win`: elapsed time is neutral or slightly worse, but CPU seconds
  per GiB improves enough to justify the tradeoff.
- `ratio-tradeoff`: performance improves while compression ratio worsens
  materially.
- `software-only-win`: a QAT-labelled row improves mostly because software did
  the work; useful for admission policy boundaries, not QAT performance.
- `regression`: latency, throughput, CPU cost, correctness, failure rate, or
  ratio worsens without an intentional compensating benefit.

## Derived CSV Columns

New `qat-phase4-benchmark.sh` runs append these derived fields:

- `cpu_count`: online CPU count used to normalize CPU seconds.
- `cpu_active_pct`: user plus system CPU percentage during the benchmark window.
- `cpu_active_s_per_gib`: active CPU seconds per GiB of source data.
- `cpu_system_s_per_gib`: system CPU seconds per GiB of source data.
- `qat_byte_share_pct`: QAT compression input bytes divided by source bytes.
- `qat_completion_share_pct`: async completions divided by async submits.
- `qat_fallback_share_pct`: async fallbacks divided by async submits.
- `qat_cap_skip_share_pct`: async cap skips divided by async submits.
- `qat_service_ns_per_mib`: QAT compression setup, submit, wait, and cleanup
  nanoseconds per QAT-completed MiB.
- `qat_wait_ns_per_mib`: QAT compression wait nanoseconds per QAT-completed MiB.
- `qat_src_buf_unaligned_64_pct`, `qat_src_buf_len_not_64_pct`,
  `qat_src_first_bytes_per_req`, and `qat_src_last_bytes_per_req`: source
  buffer alignment and first/last segment shape.
- `qat_dst_buf_unaligned_64_pct`, `qat_dst_buf_len_not_64_pct`,
  `qat_dst_first_bytes_per_req`, and `qat_dst_last_bytes_per_req`:
  destination buffer alignment and first/last segment shape.
- `qat_add_buf_unaligned_64_pct`, `qat_add_buf_len_not_64_pct`,
  `qat_add_first_bytes_per_req`, and `qat_add_last_bytes_per_req`: scratch/add
  buffer alignment and first/last segment shape.

Scale-test runs also append these hardware-state fields:

- `qat_pci_dh895xcc_count`: detected DH895XCC PCI device count.
- `qat_conf_file_count`: readable `/etc/dh895xcc_dev*.conf` file count.
- `qat_kernel_cy_instances_total`: summed `[KERNEL_QAT]` crypto instances.
- `qat_kernel_dc_instances_total`: summed `[KERNEL_QAT]` compression instances.
- `zfs_qat_dc_instances`: active QAT DC instances initialized and usable by
  ZFS, from `/proc/spl/kstat/zfs/qat`.
- `zfs_qat_dc_profile`: selected QAT compression profile.
- `zfs_qat_dc_profile_recordsize`: operator-selected target record size for
  profile decisions.
- `zfs_qat_dc_ratio_profile`: selected ratio/performance profile.
- `zfs_qat_effective_cpa_dc_level`: effective QAT compression level after
  resolving `zfs_qat_cpa_dc_level=profile`.
- `zfs_qat_effective_cpa_dc_hufftype`: effective QAT Huffman type after
  resolving `zfs_qat_cpa_dc_hufftype=profile`.
- `zfs_qat_dc_effective_max_buf_size`: effective QAT maximum buffer size after
  resolving `zfs_qat_dc_max_buf_size=profile`.
- `zfs_qat_dc_effective_async`, `zfs_qat_dc_effective_async_max_inflight`,
  `zfs_qat_dc_effective_async_submit_retries`, and
  `zfs_qat_dc_effective_async_retry_us`: effective async policy after resolving
  profile-managed async tunables.
- `zfs_qat_dc_effective_coalesce_src`,
  `zfs_qat_dc_effective_coalesce_dst`, and
  `zfs_qat_effective_decompress_disable`: effective source/destination
  coalescing and decompression policy after resolving profile-managed tunables.

Use byte share as the primary offload-share metric. Request-count share remains
useful, but it can mislead when comparing different record sizes or mixed
workloads.

## Decision Rules

- Compare QAT and software rows from the same benchmark window.
- Treat rows with nonzero cap skips as hybrid rows.
- Treat `qat_share` or `qat_completion_share_pct` as an admission/completion
  metric, not a pure hardware-utilization metric.
- Prefer `qat_byte_share_pct` over request share when deciding how much work QAT
  actually performed.
- Prefer `zfs_qat_dc_instances` over card count when interpreting cap policy.
  Card count is deployment context; active DC instances are what the ZFS QAT
  path can actually submit to.
- Require CPU and ratio evidence before enabling a policy that is not faster
  than software.
- Do not promote an HDD-only ratio win into a general default until the same
  policy has been classified against flash/NVMe results. If the media split is
  repeatable, consider profile inputs such as `rotational`, `flash`, or an
  explicit storage-bottleneck bias rather than making the operator hand-tune
  low-level QAT parameters.
- Preserve software fallback unless the benchmark intentionally measures a pure
  QAT path.
