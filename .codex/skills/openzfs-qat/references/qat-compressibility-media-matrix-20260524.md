# QAT Compressibility And Media Matrix - 2026-05-24

Purpose: add a moderate-compressibility point between the earlier highly
compressible TIFF run and the random-data run, then compare how source
compressibility changes the HDD/NVMe policy signal.

## Moderate Source

The new source alternates `64K` random blocks and `64K` zero blocks:

```text
path: /nvme_scratch/source/qat-generated/mixed-random-zero-192m.bin
bytes: 201326592
sha256: 1f448c597fa37ba30961eb3dca5ead5ae4d737e1a7c28b3980998ed3b3c778bf
observed compressratio: 1.96x
```

This gives a controlled midpoint between the TIFF workload and the random
workload. It is synthetic, so use it for policy-shape evidence rather than as a
real-world data distribution.

## Configuration

Host: `pve.drewnet.online`

```text
kernel: 7.0.0-3-pve
zfs srcversion: 636FD59ED8E9ADFD8C60AC1
QAT DC instances: 12
dc_fails after run: 0
benchmark roots: nvme_scratch/bench, test-hdd-pool/bench
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
zfs_qat_dc_async_cap_policy=profile
RUN_ORDER=record
```

Matrix:

```text
records: 512K, 1M
jobs: 1, 4, 8
iterations: 3
modes: qat, sw
verify: sw
```

Temporary `qat-phase4-*` datasets were destroyed after the run. The host was
restored to the default `balanced` profile with both benchmark roots set back to
`recordsize=128K`.

## Artifacts

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-mixed-recordorder-summary-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-mixed-media-comparison-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-compressibility-media-comparison-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-mixed-nvme-recordorder-records512k1m-jobs1-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-mixed-nvme-recordorder-records512k1m-jobs4-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-mixed-nvme-recordorder-records512k1m-jobs8-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-mixed-hdd-recordorder-records512k1m-jobs1-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-mixed-hdd-recordorder-records512k1m-jobs4-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-mixed-hdd-recordorder-records512k1m-jobs8-20260524.csv
.codex/skills/openzfs-qat/artifacts/host-root-import-20260524/zfs-qat-minbuf-mixed-media-recordorder-20260524-run.log
```

All six raw mixed-source CSVs have consistent row widths. The generated summary
uses only `row_type=raw` rows.

## Mixed-Source Results

Negative `QAT vs SW` means the QAT-labelled policy row completed faster than
same-record software gzip.

| Media | Record | Jobs | QAT ms | SW ms | QAT vs SW | QAT share | CPU QAT s/GiB | CPU SW s/GiB |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| NVMe | 512K | 1 | 823.2 | 814.5 | 1.1% slower | 87.4% | 10.05 | 33.99 |
| NVMe | 512K | 4 | 1099.2 | 1400.3 | 21.5% faster | 72.6% | 16.29 | 39.41 |
| NVMe | 512K | 8 | 1265.5 | 1828.5 | 30.8% faster | 74.4% | 15.41 | 41.02 |
| NVMe | 1M | 1 | 846.0 | 789.4 | 7.2% slower | 100.0% | 5.75 | 33.70 |
| NVMe | 1M | 4 | 1105.0 | 1303.5 | 15.2% faster | 83.6% | 11.80 | 38.64 |
| NVMe | 1M | 8 | 1466.8 | 1864.8 | 21.3% faster | 82.7% | 14.08 | 40.92 |
| HDD | 512K | 1 | 1334.3 | 1185.1 | 12.6% slower | 84.0% | 10.75 | 34.50 |
| HDD | 512K | 4 | 3187.3 | 2941.5 | 8.4% slower | 74.9% | 15.30 | 41.56 |
| HDD | 512K | 8 | 5903.6 | 5501.8 | 7.3% slower | 76.3% | 15.67 | 42.02 |
| HDD | 1M | 1 | 1373.3 | 1186.9 | 15.7% slower | 100.0% | 6.10 | 35.62 |
| HDD | 1M | 4 | 3173.8 | 2962.5 | 7.1% slower | 83.3% | 12.23 | 41.10 |
| HDD | 1M | 8 | 5843.0 | 5583.2 | 4.7% slower | 84.0% | 12.64 | 42.23 |

## Cross-Source Pattern

The three source classes now show distinct behavior:

| Source class | Ratio | NVMe pattern | HDD pattern |
|---|---:|---|---|
| Highly compressible TIFF | ~16.9x to ~26.0x | `512K` wins; `1M` loses in paired NVMe record-order testing | `512K` wins; `1M` wins at jobs 4 and 8 |
| Mixed random/zero | 1.96x | QAT wins at jobs 4 and 8, loses at jobs 1 | QAT loses elapsed time at all tested rows |
| Random | 1.00x | QAT wins strongly at all tested rows | QAT is roughly neutral to slower except `1M` jobs 1 |

Interpretation:

- CPU offload is consistently real. QAT/hybrid reduces active CPU seconds per
  GiB across all three source classes and both media classes.
- Elapsed-time benefit is not determined by media type alone. At `1.96x`, HDD
  still lost elapsed time even though the write volume was roughly halved.
- The earlier highly compressible HDD win is more consistent with a
  media-plus-ratio interaction than with a standalone rotational-media rule.
- NVMe benefits from QAT when CPU is the bottleneck, including incompressible
  random data where software gzip wastes CPU and compressed byte volume does
  not shrink.
- HDD only appears to benefit in elapsed time when the compression ratio is
  high enough to materially reduce device work beyond the QAT request overhead.

## Profile Implication

Do not add a simple `rotational` or `flash` profile flag yet. The useful signal
is at least:

```text
storage media class + expected compression ratio + target record size + concurrency
```

The first implementation of this signal is
`zfs_qat_dc_expected_ratio=unknown|low|medium|high`. `unknown` preserves the
previous profile behavior. Explicit values are host-level hints that only affect
profile-managed tunables:

- `low` treats QAT as primarily a CPU-offload path for poorly-compressible data.
- `medium` avoids the measured moderate-compressibility `1M` elapsed-time
  regression by capping profile-managed QAT records at `512K`.
- `high` allows higher compression effort when the balanced ratio profile is
  otherwise selected.

A storage-media profile may still be useful later, but only as a modifier after
the expected-ratio policy decides whether larger records or higher compression
effort are likely to pay off.
