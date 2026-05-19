# Post-DKMS QAT profile matrix - 2026-05-19

Purpose: establish the post-DKMS performance baseline before making additional QAT engine or policy changes.

Raw data:

- `zfs-qat-post-dkms-target128k-jobs1-20260519.csv`
- `zfs-qat-post-dkms-target128k-jobs4-20260519.csv`
- `zfs-qat-post-dkms-target128k-jobs8-20260519.csv`
- `zfs-qat-post-dkms-target1m-jobs1-20260519.csv`
- `zfs-qat-post-dkms-target1m-jobs4-20260519.csv`
- `zfs-qat-post-dkms-target1m-jobs8-20260519.csv`

## Method

- Host: `pve.drewnet.online`
- Kernel: `7.0.0-3-pve`
- QAT DKMS submodule: `0a3fc14`
- OpenZFS branch: `b5bed1147`
- QAT devices: 2 x DH895XCC
- ZFS-visible DC instances: 12
- Profiles: `zfs_qat_dc_profile=balanced`, `zfs_qat_dc_ratio_profile=balanced`
- Profile targets: `zfs_qat_dc_profile_recordsize=131072` and `1048576`
- Records: `128K`, `256K`, `512K`, `1M`
- Jobs: `1`, `4`, `8`
- Modes: QAT enabled and QAT disabled software baseline
- Iterations: 3 per cell
- Source: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`

All raw iterations completed with `sha_ok=yes` and `dc_fails_delta=0`.

Changing `zfs_qat_dc_profile_recordsize` from 128K to 1M returned `EBUSY` after QAT DC initialization because `zfs_qat_dc_max_buf_size=profile` was already active. The 1M target matrix therefore required a temporary boot-time module option and reboot. The host was restored to the 128K default profile after the run.

## Summary

Positive `QAT vs SW` means QAT completed faster than software gzip. Negative means QAT was slower. CPU reduction compares active CPU percentage for the QAT-mode run against the matching software run.

| Target | Jobs | Record | QAT ms | SW ms | QAT vs SW | QAT CPU active | SW CPU active | CPU reduction | QAT byte share | QAT service ms/MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 128K | 1 | 128K | 799.0 | 758.5 | -5.1% | 1.48% | 4.14% | 64.2% | 100.0% | 6.33 |
| 128K | 1 | 256K | 653.5 | 631.0 | -3.4% | 4.20% | 4.44% | 5.5% | 0.0% |  |
| 128K | 1 | 512K | 575.0 | 570.5 | -0.8% | 4.96% | 4.73% | -4.9% | 0.0% |  |
| 128K | 1 | 1M | 590.6 | 552.6 | -6.4% | 4.60% | 5.03% | 8.6% | 0.0% |  |
| 128K | 4 | 128K | 1118.8 | 1090.2 | -2.6% | 3.73% | 13.14% | 71.6% | 100.0% | 10.91 |
| 128K | 4 | 256K | 992.6 | 988.2 | -0.4% | 13.32% | 13.49% | 1.2% | 0.0% |  |
| 128K | 4 | 512K | 1009.1 | 955.0 | -5.4% | 12.90% | 13.48% | 4.3% | 0.0% |  |
| 128K | 4 | 1M | 1034.3 | 919.8 | -11.1% | 12.97% | 13.64% | 4.9% | 0.0% |  |
| 128K | 8 | 128K | 1616.5 | 1496.9 | -7.4% | 5.21% | 19.82% | 73.7% | 100.0% | 10.74 |
| 128K | 8 | 256K | 1478.7 | 1334.8 | -9.7% | 18.37% | 20.56% | 10.7% | 0.0% |  |
| 128K | 8 | 512K | 1400.0 | 1405.6 | +0.4% | 18.98% | 19.30% | 1.7% | 0.0% |  |
| 128K | 8 | 1M | 1555.6 | 1473.8 | -5.3% | 18.86% | 19.42% | 2.9% | 0.0% |  |
| 1M | 1 | 128K | 819.1 | 743.4 | -9.2% | 1.36% | 4.26% | 68.2% | 100.0% | 6.17 |
| 1M | 1 | 256K | 682.8 | 623.0 | -8.8% | 1.28% | 4.32% | 70.3% | 100.0% | 6.23 |
| 1M | 1 | 512K | 552.5 | 559.8 | +1.3% | 1.23% | 4.72% | 73.9% | 100.0% | 6.21 |
| 1M | 1 | 1M | 552.5 | 563.9 | +2.1% | 1.11% | 4.46% | 75.1% | 100.3% | 6.27 |
| 1M | 4 | 128K | 1157.8 | 1097.4 | -5.2% | 3.98% | 12.93% | 69.2% | 100.0% | 10.11 |
| 1M | 4 | 256K | 1076.4 | 999.9 | -7.1% | 3.10% | 13.38% | 76.8% | 100.0% | 10.99 |
| 1M | 4 | 512K | 1045.6 | 960.5 | -8.1% | 2.74% | 13.43% | 79.6% | 100.0% | 10.96 |
| 1M | 4 | 1M | 1038.6 | 914.4 | -12.0% | 2.37% | 13.36% | 82.3% | 100.3% | 10.69 |
| 1M | 8 | 128K | 1565.5 | 1440.7 | -8.0% | 5.40% | 19.40% | 72.1% | 100.1% | 10.69 |
| 1M | 8 | 256K | 1486.6 | 1399.3 | -5.9% | 4.35% | 19.28% | 77.4% | 100.1% | 10.83 |
| 1M | 8 | 512K | 1482.6 | 1393.7 | -6.0% | 3.75% | 19.05% | 80.3% | 100.3% | 10.86 |
| 1M | 8 | 1M | 1472.7 | 1377.2 | -6.5% | 3.39% | 19.25% | 82.4% | 100.8% | 10.36 |

## Findings

- The 128K target policy works as intended: only 128K records are offloaded to QAT, while 256K and larger records fall back to software.
- With the 128K target, QAT 128K saves substantial CPU at every concurrency level, but remains slower than software gzip by `2.6%` to `7.4%`.
- The 1M target policy enables QAT offload for all tested record sizes.
- With the 1M target and one job, QAT reaches slight elapsed-time wins at 512K and 1M while reducing active CPU by roughly `74%` to `75%`.
- With the 1M target and four or eight jobs, QAT remains slower than software for every tested record size, despite reducing active CPU by roughly `69%` to `82%`.
- QAT service time is about `6.2 ms/MiB` at one job and about `10 ms/MiB` to `11 ms/MiB` at four or eight jobs. The concurrency penalty points at queueing/wait behavior or driver submission/completion overhead, not just fixed per-record setup.

## Next target

The next useful work is instrumentation across the QAT driver boundary. ZFS already shows high wait/service time under concurrency, but it does not tell us whether the time is in ZFS glue, the QAT kernel API path, software queueing, polling/interrupt behavior, or actual hardware service. Add QAT-driver-side counters around DC enqueue, dequeue/completion, retries, and callback paths, then compare those timings to the existing ZFS `submit`, `wait`, and `cleanup` counters.
