# QAT Shape Stats Gating - 2026-05-25

## Change

Detailed per-flat-buffer QAT compression shape accounting is now opt-in through:

```text
zfs_qat_dc_shape_stats=profile|0|1
```

Profile currently resolves to `0`.

Follow-up: `references/qat-aggregate-shape-stats-gating-20260527.md` later
gated these aggregate request-shape counters behind the same parameter:

- `dc_compress_src_buffers`
- `dc_compress_dst_buffers`
- `dc_compress_add_buffers`
- `dc_compress_dst_total_buffers`
- max-buffer counters

The gated detailed counters are the per-buffer alignment and first/last-size counters:

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

This avoids calling `qat_dc_note_flat_buffer()` for every source, destination,
and scratch flat buffer during normal operation. At `1M`, the current request
shape is about `256` source buffers plus `289` destination/scratch buffers per
QAT request, so these detailed diagnostic calls scale directly with record size.

## Validation

Local checks:

- `git diff --check`: pass
- `bash -n .codex/skills/openzfs-qat/scripts/qat-phase4-benchmark.sh`: pass

Host deployment:

- Synced source to `pve.drewnet.online:/usr/src/zfs-2.4.99/`
- Rebuilt and installed ZFS DKMS against `/usr/src/qat-4.28.0-00004`
- Ran `depmod -a` and `update-initramfs -u -k 7.0.0-3-pve`
- Rebooted into `7.0.0-3-pve`
- Verified `zfs_qat_dc_shape_stats=profile`, `qat.service` active, pools healthy, and no QAT failures

Runtime parameter check:

```text
initial=profile
manual_on=1
manual_off=0
profile=profile
```

## Benchmark

Artifacts:

- `artifacts/shape-stats-gating-20260525/zfs-qat-shape-stats-off-rerun-1m-jobs4-20260525.csv`
- `artifacts/shape-stats-gating-20260525/zfs-qat-shape-stats-off-rerun-1m-jobs8-20260525.csv`
- `artifacts/shape-stats-gating-20260525/zfs-qat-shape-stats-on-rerun-1m-jobs4-20260525.csv`
- `artifacts/shape-stats-gating-20260525/zfs-qat-shape-stats-on-rerun-1m-jobs8-20260525.csv`
- `artifacts/shape-stats-gating-20260525/summary.csv`

Benchmark shape:

- Pool: `nvme_scratch`
- Source: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`
- Record size: `1M`
- Jobs: `4`, `8`
- Iterations: `3`
- Mode: `qat`
- Verify mode: `sw`
- Temporary boot profile: `zfs_qat_dc_profile=throughput`, `zfs_qat_dc_profile_recordsize=1048576`, `zfs_qat_dc_async=1`, `zfs_qat_dc_async_max_inflight=96`, `zfs_qat_dc_async_cap_policy=throughput`

The first shape-stats benchmark attempt exposed a CSV alignment bug in the
benchmark harness: the header had the new shape-stat columns, but the raw row
array did not. The harness was fixed and all retained rerun artifacts validate
as `254` fields per row.

## Results

| Shape stats | Jobs | Mean elapsed ms | Mean write MiB/s | Mean CPU active s/GiB | QAT byte share | Setup ns/request | Submit ns/request | Request prepare ns/request | Detailed src-first bytes/request |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| off/profile | 4 | 784.010 | 932.270 | 5.093 | 87.590% | 95352.376 | 38000.947 | 1091.359 | 0.000 |
| on | 4 | 763.366 | 956.517 | 5.223 | 84.163% | 46809.956 | 38454.260 | 923.344 | 4096.000 |
| off/profile | 8 | 966.101 | 1511.433 | 6.663 | 81.310% | 47436.981 | 39825.385 | 1101.949 | 0.000 |
| on | 8 | 867.033 | 1687.910 | 5.990 | 77.840% | 48627.652 | 39711.356 | 1099.824 | 4096.000 |

## Interpretation

The benchmark proves the behavioral split:

- `profile`/off suppresses the detailed per-buffer shape counters.
- `1` restores the detailed counters, shown by `4096` first-source-byte measurements for the page-aligned 1M source buffers.
- Aggregate source/destination/add buffer-count counters remain populated in both modes.

The elapsed-time result should not be used as proof of an end-to-end win. QAT
byte share and wait time moved between paired runs, and wait time still
dominates service cost. The value of this change is that normal operation no
longer pays per-flat-buffer diagnostic accounting cost unless an operator or
benchmark explicitly enables it.

## Usage

Use profile/default for normal performance runs:

```text
zfs_qat_dc_shape_stats=profile
```

Enable detailed shape diagnostics only when investigating alignment or segment
shape:

```text
zfs_qat_dc_shape_stats=1
```

When detailed shape stats are disabled, benchmark fields such as
`qat_src_first_bytes_per_req`, `qat_dst_first_bytes_per_req`, and alignment
percentages will be zero or `na`. This is expected and should not be interpreted
as an aligned/unproblematic request shape.
