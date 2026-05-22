# Alignment Shape Instrumentation - 2026-05-22

Purpose: add low-overhead observability for QAT compression buffer alignment and
segment shape before considering any targeted copy/coalescing policy.

This follows the source-coalescing result: broad source coalescing reduced
scatter/gather count, but copy cost erased the benefit. The next question was
whether a narrower copy path could help only when source or destination buffers
are badly aligned.

## Implementation

Added QAT compression kstats for source, destination, and scratch/add buffers:

- `dc_compress_src_buf_unaligned_64`
- `dc_compress_src_buf_len_not_64`
- `dc_compress_src_first_bytes`
- `dc_compress_src_last_bytes`
- `dc_compress_dst_buf_unaligned_64`
- `dc_compress_dst_buf_len_not_64`
- `dc_compress_dst_first_bytes`
- `dc_compress_dst_last_bytes`
- `dc_compress_add_buf_unaligned_64`
- `dc_compress_add_buf_len_not_64`
- `dc_compress_add_first_bytes`
- `dc_compress_add_last_bytes`

The implementation accumulates these values locally while building each QAT
compression request's buffer lists, then publishes one set of kstat increments
per submitted compression request. It does not add per-buffer atomic operations.

The phase-4 benchmark CSV now also reports derived per-request fields:

- `qat_src_buf_unaligned_64_pct`
- `qat_src_buf_len_not_64_pct`
- `qat_src_first_bytes_per_req`
- `qat_src_last_bytes_per_req`
- `qat_dst_buf_unaligned_64_pct`
- `qat_dst_buf_len_not_64_pct`
- `qat_dst_first_bytes_per_req`
- `qat_dst_last_bytes_per_req`
- `qat_add_buf_unaligned_64_pct`
- `qat_add_buf_len_not_64_pct`
- `qat_add_first_bytes_per_req`
- `qat_add_last_bytes_per_req`

## Build And Load Validation

Host: `pve.drewnet.online`

Build logs:

```text
/root/zfs-qat-alignment-kstats-dkms-build-20260522.log
/root/zfs-qat-alignment-kstats-dkms-install-20260522.log
```

Loaded module after DKMS install, initramfs update, and reboot:

```text
filename: /lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko
version: 2.4.99-1
srcversion: 38C562CCF5F59E6889FDE50
```

Runtime health after final restore:

```text
zfs_qat_dc_profile=balanced
zfs_qat_dc_profile_recordsize=131072
zfs_qat_dc_max_buf_size=profile
zfs_qat_dc_coalesce_src=profile
zfs_qat_dc_coalesce_dst=profile
zfs_qat_decompress_disable=profile
zfs_qat_dc_poll=profile
zfs_qat_compress_disable=0
dc_instances=12
dc_watchdog_health=1
dc_watchdog_runtime_disables=0
dc_compress_async_inflight=0
qat.service=active
```

## Smoke Benchmark

The host was temporarily booted with:

```text
zfs_qat_dc_profile_recordsize=1048576
zfs_qat_dc_max_buf_size=profile
zfs_qat_decompress_disable=1
```

This allowed `1M` to be tested as an actual QAT-eligible record size. The host
was restored to the normal `128K` profile target after the benchmark.

Artifacts:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-alignment-shape-sync-jobs1-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-alignment-shape-async-jobs4-20260522.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-alignment-shape-summary-20260522.csv
```

CSV validation:

```text
sync: 244 columns, 8 rows, alignment OK
async: 244 columns, 8 rows, alignment OK
```

## Observed Shape

QAT rows only:

| Profile | Jobs | Record | QAT Byte Share | Src Buffers/Req | Dst Total Buffers/Req | Src Unaligned | Src Len Not 64 | Dst Unaligned | Dst Len Not 64 | Add Unaligned | Add Len Not 64 | Add Last Bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| balanced sync | 1 | 128K | 100.01% | 32 | 37 | 0.00% | 0.00% | 0.00% | 0.00% | 0.00% | 11.11% | 57 |
| balanced sync | 1 | 1M | 100.28% | 256 | 289 | 0.00% | 0.00% | 0.00% | 0.00% | 0.00% | 1.54% | 57 |
| throughput async | 4 | 128K | 68.60% | 32 | 37 | 0.00% | 0.00% | 0.00% | 0.00% | 0.00% | 11.11% | 57 |
| throughput async | 4 | 1M | 72.47% | 256 | 289 | 0.00% | 0.00% | 0.00% | 0.00% | 0.00% | 1.54% | 57 |

Interpretation:

- Source buffers were already 64-byte aligned.
- Source buffer lengths were already multiples of 64 bytes.
- Destination buffers were already 64-byte aligned.
- Destination buffer lengths were already multiples of 64 bytes.
- Scratch/add buffers were 64-byte aligned, but their final tail segment was
  not a 64-byte multiple. The observed final scratch segment was `57` bytes.

## Decision

Do not implement a targeted source or destination alignment-copy policy from
this data.

The measured source and destination alignment shape does not explain QAT service
latency on this workload. A copy policy aimed at source or destination alignment
would add CPU and memory traffic without fixing an observed defect.

Do not implement a scratch-tail copy policy either. The scratch buffer exists to
absorb expansion and avoid overflow; the unaligned final scratch tail is small,
expected from the bound size, and does not justify another copy path without
direct evidence that QAT is sensitive to that tail.

## Next Target

The best next target is QAT-side/platform tuning rather than another ZFS copy
path:

- Verify PCIe link width and speed for both DH895XCC cards.
- Capture QAT kernel instance service configuration after the DC-only changes.
- Re-check QAT polling/interrupt mode and driver timing counters with the
  alignment counters present.
- Evaluate QAT driver parameter-checking options only as a controlled
  correctness-tested experiment.

Keep the alignment counters in place for future workloads. If another workload
shows source or destination misalignment, a targeted copy experiment can be
reopened with evidence.
