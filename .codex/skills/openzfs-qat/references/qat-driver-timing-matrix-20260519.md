# QAT Driver Timing Matrix - 2026-05-19

## Purpose

Measure QAT driver-side timing after adding `qat_api.ko` traditional DC timing
counters. This run was intended to separate ZFS wrapper cost from QAT
driver/device response wait.

## Test Setup

- Host: `pve.drewnet.online`
- Cards: 2x `dh895xcc`
- Kernel: `7.0.0-3-pve`
- QAT driver: DKMS `qat/4.28.0-00004`
- ZFS module: DKMS `zfs/2.4.99`
- Source file: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`
- Source size: `191,346,108` bytes
- Harness: `/root/qat-phase4-benchmark.sh`
- Command shape: `ITERS=3 RECORDS="128K 256K 512K 1M" MODES="qat sw" VERIFY_MODE=sw`
- Temporary boot setting: `zfs_qat_dc_profile_recordsize=1048576`

The temporary boot setting was required because the host default
`zfs_qat_dc_profile_recordsize=131072` makes `zfs_qat_dc_max_buf_size=profile`
fall back to software above 128K. The host was restored to the default 128K
profile recordsize after the matrix.

## Artifacts

```text
/root/zfs-qat-driver-timing-matrix-1mprofile-jobs1-20260519-r2.csv
/root/zfs-qat-driver-timing-matrix-1mprofile-jobs4-20260519-r2.csv
/root/zfs-qat-driver-timing-matrix-1mprofile-jobs8-20260519-r2.csv
```

Repo copies:

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-driver-timing-matrix-1mprofile-jobs1-20260519-r2.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-driver-timing-matrix-1mprofile-jobs4-20260519-r2.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-driver-timing-matrix-1mprofile-jobs8-20260519-r2.csv
```

## Result Summary

`qat_vs_sw_pct` is elapsed time versus software. Negative is faster than
software; positive is slower.

| Jobs | Record | QAT avg ms | SW avg ms | QAT vs SW | QAT MiB/s | SW MiB/s | QAT CPU active | SW CPU active | QAT requests |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 128K | 797.386 | 783.905 | +1.7% | 229.03 | 233.48 | 1.74% | 3.91% | 1,460 |
| 1 | 256K | 659.437 | 665.228 | -0.9% | 279.48 | 275.44 | 1.68% | 4.30% | 730 |
| 1 | 512K | 608.860 | 590.047 | +3.2% | 299.82 | 309.44 | 1.68% | 4.79% | 365 |
| 1 | 1M | 573.236 | 571.204 | +0.4% | 318.46 | 319.56 | 1.57% | 4.89% | 183 |
| 4 | 128K | 1110.741 | 1062.752 | +4.5% | 657.60 | 688.09 | 5.03% | 13.81% | 5,840 |
| 4 | 256K | 1047.913 | 1002.812 | +4.5% | 697.59 | 728.10 | 4.43% | 13.68% | 2,920 |
| 4 | 512K | 1051.493 | 970.228 | +8.4% | 694.22 | 752.42 | 4.31% | 14.09% | 1,460 |
| 4 | 1M | 1046.538 | 937.209 | +11.7% | 700.69 | 779.09 | 3.99% | 13.98% | 732 |
| 8 | 128K | 1522.270 | 1462.455 | +4.1% | 960.75 | 998.88 | 7.85% | 19.10% | 11,686 |
| 8 | 256K | 1442.865 | 1342.140 | +7.5% | 1011.91 | 1088.04 | 6.74% | 20.21% | 5,847 |
| 8 | 512K | 1443.839 | 1424.171 | +1.4% | 1011.83 | 1028.10 | 5.99% | 18.68% | 2,927 |
| 8 | 1M | 1496.038 | 1413.287 | +5.9% | 976.97 | 1033.67 | 5.72% | 19.29% | 1,471 |

## Driver Timing Summary

Average driver response wait is almost the same as average driver total time,
which means the dominant cost is below the ZFS wrapper and inside the
QAT request/response path.

| Jobs | Record | Avg driver wait | Avg driver total | Driver wait per MiB | ZFS wait per MiB |
|---:|---:|---:|---:|---:|---:|
| 1 | 128K | 740.4 us | 747.6 us | 5,922,885 ns/MiB | 5,995,837 ns/MiB |
| 1 | 256K | 1481.7 us | 1491.9 us | 5,926,721 ns/MiB | 5,975,783 ns/MiB |
| 1 | 512K | 2957.9 us | 2974.4 us | 5,915,759 ns/MiB | 5,959,266 ns/MiB |
| 1 | 1M | 5938.4 us | 5965.9 us | 5,938,365 ns/MiB | 5,978,328 ns/MiB |
| 4 | 128K | 1338.5 us | 1345.8 us | 10,708,226 ns/MiB | 10,805,512 ns/MiB |
| 4 | 256K | 2714.9 us | 2725.0 us | 10,859,535 ns/MiB | 10,917,810 ns/MiB |
| 4 | 512K | 5314.4 us | 5330.8 us | 10,628,777 ns/MiB | 10,666,223 ns/MiB |
| 4 | 1M | 10506.1 us | 10533.4 us | 10,506,108 ns/MiB | 10,536,950 ns/MiB |
| 8 | 128K | 1312.0 us | 1319.9 us | 10,496,127 ns/MiB | 10,586,392 ns/MiB |
| 8 | 256K | 2582.5 us | 2593.2 us | 10,329,919 ns/MiB | 10,386,046 ns/MiB |
| 8 | 512K | 5398.8 us | 5415.3 us | 10,797,611 ns/MiB | 10,839,975 ns/MiB |
| 8 | 1M | 10444.9 us | 10472.7 us | 10,444,769 ns/MiB | 10,475,518 ns/MiB |

## Interpretation

- QAT offloaded all tested record sizes in the corrected matrix.
- QAT remained slower than software in every concurrent row and was only
  marginally faster for the single-job 256K row.
- QAT used much less active CPU than software. At `JOBS=4`, QAT active CPU was
  roughly 4-5% versus software around 14%; at `JOBS=8`, QAT active CPU was
  roughly 6-8% versus software around 19-20%.
- The driver wait per MiB is stable within each concurrency level: about
  5.9 ms/MiB at `JOBS=1` and about 10.3-10.9 ms/MiB at `JOBS=4/8`.
- ZFS wait-per-MiB closely tracks driver wait-per-MiB, so ZFS wrapper overhead
  is not the primary latency source for these rows.

## Next Target

The next optimization target should be QAT driver/runtime behavior, not more
ZFS-side allocation work. Specifically inspect and test QAT 1.x-supported
polling versus interrupt behavior, response delivery configuration, ring/bank
service affinity, and DC instance/ring distribution. Use
`qat_driver_response_wait_ns_delta`, `qat_driver_avg_response_wait_ns`, elapsed
time, and CPU active percentage as the primary decision metrics.
