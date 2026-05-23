# QAT Min-Buffer NVMe Paired Run-Order Validation - 2026-05-23

Purpose: rerun the NVMe-backed min-buffer policy matrix with
`RUN_ORDER=record`, so QAT and software rows for each record size are measured
in adjacent benchmark windows.

## Configuration

Host: `pve.drewnet.online`

```text
kernel: 7.0.0-3-pve
zfs srcversion: 636FD59ED8E9ADFD8C60AC1
QAT DC instances: 12
dc_fails: 0
benchmark root: nvme_scratch/bench
```

Runtime policy during the run:

```text
zfs_qat_dc_profile=throughput
zfs_qat_dc_ratio_profile=balanced
zfs_qat_dc_profile_recordsize=1048576
zfs_qat_dc_min_buf_size=profile
zfs_qat_dc_effective_min_buf_size=524288
zfs_qat_dc_max_buf_size=profile
zfs_qat_dc_effective_max_buf_size=1048576
RUN_ORDER=record
```

Matrix:

```text
records: 128K, 256K, 512K, 1M
jobs: 1, 4, 8
iterations: 3
modes: qat, sw
verify: sw
```

The host was restored to the default balanced `128K` profile after testing and
temporary `nvme_scratch/bench/qat-phase4-*` datasets were destroyed.

## Artifacts

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-nvme-recordorder-summary-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-nvme-recordorder-records128k256k512k1m-jobs1-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-nvme-recordorder-records128k256k512k1m-jobs4-20260523.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-nvme-recordorder-records128k256k512k1m-jobs8-20260523.csv
.codex/skills/openzfs-qat/artifacts/host-root-import-20260523/zfs-qat-minbuf-nvme-recordorder-20260523-run.log
```

All three raw CSVs have 246 columns on every row.

## Results

Negative `QAT vs SW` means the QAT-labelled policy row completed faster than
same-record software gzip. For `128K` and `256K`, QAT share is zero by design
because the throughput profile skips QAT below the `512K` minimum.

| Record | Jobs | QAT ms | SW ms | QAT vs SW | QAT share | CPU QAT s/GiB | CPU SW s/GiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| 128K | 1 | 568.3 | 560.5 | 1.4% slower | 0.0% | 10.56 | 9.66 |
| 128K | 4 | 774.2 | 798.8 | 3.1% faster | 0.0% | 11.05 | 11.15 |
| 128K | 8 | 918.0 | 866.4 | 6.0% slower | 0.0% | 12.20 | 11.91 |
| 256K | 1 | 547.7 | 534.4 | 2.5% slower | 0.0% | 9.38 | 9.49 |
| 256K | 4 | 778.4 | 786.8 | 1.1% faster | 0.0% | 11.13 | 11.11 |
| 256K | 8 | 870.6 | 844.3 | 3.1% slower | 0.0% | 12.05 | 11.58 |
| 512K | 1 | 507.2 | 530.9 | 4.5% faster | 75.6% | 4.77 | 9.70 |
| 512K | 4 | 732.1 | 793.5 | 7.7% faster | 45.4% | 7.58 | 11.32 |
| 512K | 8 | 827.8 | 880.1 | 5.9% faster | 53.7% | 7.54 | 11.76 |
| 1M | 1 | 534.6 | 520.6 | 2.7% slower | 100.3% | 3.29 | 9.40 |
| 1M | 4 | 802.5 | 752.0 | 6.7% slower | 81.2% | 5.39 | 11.22 |
| 1M | 8 | 914.8 | 874.0 | 4.7% slower | 74.9% | 6.12 | 12.56 |

## Interpretation

- `512K` is the only record size that wins elapsed time at every tested
  concurrency level in this paired run.
- `512K` also materially reduces active CPU cost versus software gzip, even
  when QAT byte share is only about half of the input due to cap skips.
- `1M` is a CPU-offload win but not an elapsed-time win in this pass. It saved
  roughly 35-65% active CPU seconds per GiB, but was slower than software at
  jobs `1`, `4`, and `8`.
- `128K` and `256K` remain policy-skip rows. Their small wins/losses should be
  treated as benchmark variance and software-only policy-window checks, not QAT
  engine evidence.
- Compared with the previous `RUN_ORDER=mode` pass, the suspicious
  `256K/jobs=8` software-only win disappeared. This supports using
  `RUN_ORDER=record` for future same-record policy comparisons.

## Storage-Media Follow-Up

The NVMe result should not be assumed to generalize to HDD pools. A
higher-ratio policy may perform better on slower rotational media if fewer
compressed bytes pass through the device bottleneck. Before adding a
`rotational`/`flash` profile input, rerun the same paired method on
`test-hdd-pool` and classify whether any win is driven by elapsed time, CPU
offload, compression ratio, or reduced write bandwidth.

Status: completed on 2026-05-23. See
`references/qat-minbuf-hdd-recordorder-20260523.md`.

## Next Target

Use `RUN_ORDER=record` by default for future phase 4 profile comparisons. The
benchmark harness now defaults to this paired order while still accepting
`RUN_ORDER=mode` for legacy mode-sweep ordering.
Before adding a media-bias profile input, repeat the HDD/NVMe comparison with
other source data and at least one less-compressible workload.
