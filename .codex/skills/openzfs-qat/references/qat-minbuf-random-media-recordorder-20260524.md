# QAT Min-Buffer Random-Data Media Check - 2026-05-24

Purpose: check whether the HDD/NVMe split seen with highly compressible TIFF
data repeats on a less-compressible source. This run used synthetic random data
so compression ratio stayed at `1.00x`; it is therefore a CPU-offload and
admission-policy check, not a reduced-write-volume check.

## Configuration

Host: `pve.drewnet.online`

```text
kernel: 7.0.0-3-pve
zfs srcversion: 636FD59ED8E9ADFD8C60AC1
QAT DC instances: 12
dc_fails before restore: 0
source: /nvme_scratch/source/qat-generated/random-192m.bin
source bytes: 201326592
source sha256: ee19e0b754540770fd85a592ad2a111046e5cee404a594efe29edd5e3dc410fd
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
restored to `zfs_qat_dc_profile=balanced`, `zfs_qat_dc_ratio_profile=balanced`,
`zfs_qat_dc_min_buf_size=profile`, `zfs_qat_dc_max_buf_size=profile`, and
`recordsize=128K` on `nvme_scratch/bench` after testing.

## Artifacts

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-random-recordorder-summary-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-random-media-comparison-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-random-nvme-recordorder-records512k1m-jobs1-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-random-nvme-recordorder-records512k1m-jobs4-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-random-nvme-recordorder-records512k1m-jobs8-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-random-hdd-recordorder-records512k1m-jobs1-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-random-hdd-recordorder-records512k1m-jobs4-20260524.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-minbuf-random-hdd-recordorder-records512k1m-jobs8-20260524.csv
.codex/skills/openzfs-qat/artifacts/host-root-import-20260524/zfs-qat-minbuf-random-media-recordorder-20260524-run.log
```

All six raw CSVs have consistent row widths. The generated summary uses only
`row_type=raw` rows.

## Results

Negative `QAT vs SW` means the QAT-labelled policy row completed faster than
same-record software gzip. `QAT share` is the byte-share handled by QAT before
hybrid software fallback.

| Media | Record | Jobs | QAT ms | SW ms | QAT vs SW | QAT share | CPU QAT s/GiB | CPU SW s/GiB | Ratio |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| NVMe | 512K | 1 | 437.2 | 725.9 | 39.8% faster | 86.6% | 12.36 | 54.90 | 1.00 |
| NVMe | 512K | 4 | 901.6 | 1447.7 | 37.7% faster | 75.4% | 20.72 | 65.40 | 1.00 |
| NVMe | 512K | 8 | 1223.4 | 2359.4 | 48.2% faster | 76.5% | 19.31 | 65.08 | 1.00 |
| NVMe | 1M | 1 | 421.1 | 720.3 | 41.5% faster | 100.0% | 3.62 | 53.30 | 1.00 |
| NVMe | 1M | 4 | 797.6 | 1386.3 | 42.5% faster | 80.1% | 15.81 | 64.43 | 1.00 |
| NVMe | 1M | 8 | 1349.9 | 2336.5 | 42.2% faster | 79.8% | 21.14 | 65.54 | 1.00 |
| HDD | 512K | 1 | 1742.0 | 1686.2 | 3.3% slower | 81.5% | 14.17 | 54.78 | 1.00 |
| HDD | 512K | 4 | 5042.9 | 5032.9 | 0.2% slower | 75.0% | 19.87 | 63.59 | 1.00 |
| HDD | 512K | 8 | 9850.4 | 9619.1 | 2.4% slower | 76.1% | 20.02 | 65.15 | 1.00 |
| HDD | 1M | 1 | 1522.7 | 1566.2 | 2.8% faster | 100.0% | 4.57 | 55.99 | 1.00 |
| HDD | 1M | 4 | 5027.1 | 4796.9 | 4.8% slower | 80.3% | 16.49 | 65.72 | 1.00 |
| HDD | 1M | 8 | 9715.2 | 9595.2 | 1.3% slower | 81.7% | 16.64 | 66.01 | 1.00 |

## Interpretation

- Random data did not repeat the earlier compressible-data `1M` HDD win at
  higher concurrency. The only HDD elapsed-time win was `1M` at `jobs=1`.
- The earlier TIFF result is therefore more likely tied to compression ratio
  and reduced write volume than to HDD media alone.
- QAT/hybrid strongly reduced CPU active seconds per GiB on both media classes.
  On NVMe this also translated into large elapsed-time wins because software
  gzip spent CPU compressing incompressible data.
- On HDD, elapsed time was dominated by the storage path when write volume did
  not shrink, so CPU offload did not materially improve wall-clock time.
- This result supports keeping media bias as a future profile input only when
  paired with ratio or byte-reduction evidence. Random incompressible data
  supports QAT admission/fallback policy and data-compressibility profiling; it
  does not justify a standalone `rotational` versus `flash` default split.

## Next Target

Keep the current `512K` throughput-profile minimum as the safest broad default
candidate. Do not promote a storage-media profile flag from this run alone.
The 2026-05-24 mixed random/zero run added the moderate-compressibility point;
see `qat-compressibility-media-matrix-20260524.md`. That result reinforces that
storage media should be a modifier only after expected compression ratio is
known or operator-supplied.
