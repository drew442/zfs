# QAT DKMS regression benchmark - 2026-05-19

Purpose: confirm the DKMS-installed QAT 4.28 driver and ZFS DKMS rebuild still provide working QAT compression on `pve.drewnet.online`.

Raw data: `zfs-qat-dkms-regression-20260519.csv`

## Host state

- Host: `pve.drewnet.online`
- Kernel: `7.0.0-3-pve`
- QAT DKMS: `qat/4.28.0-00004`
- ZFS DKMS: `zfs/2.4.99`
- QAT devices: 2 x `dh895xcc`
- DC instances visible to ZFS: 12
- QAT profile: `balanced`
- QAT profile recordsize: `131072`
- Ratio profile: `balanced`
- Jobs per run: 4
- Iterations per mode/recordsize: 2
- Source file: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`
- Source bytes per run: 765,384,432

## Results

| Mode | Recordsize | Avg elapsed ms | Avg write MiB/s | Avg CPU active % | Compression ratio | QAT byte share | QAT requests | QAT failures | SHA |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| QAT | 128K | 1202.227 | 607.43 | 3.57 | 16.92x | 100.01% | 5840 | 0 | yes |
| SW | 128K | 1098.239 | 664.92 | 12.62 | 16.90x | 0.00% | 0 | 0 | yes |
| QAT mode | 1M | 942.304 | 774.75 | 13.56 | 25.26x | 0.00% | 0 | 0 | yes |
| SW | 1M | 979.609 | 745.14 | 13.28 | 25.26x | 0.00% | 0 | 0 | yes |

## Interpretation

- DKMS-installed QAT is operational: the 128K QAT run completed with zero `dc_fails`, checksum validation passed, and essentially all compressed bytes were processed by QAT.
- The 128K QAT run reduced CPU active time materially versus software gzip (`3.57%` vs `12.62%`) while preserving compression ratio.
- The 128K QAT run remained slower than software gzip in elapsed time in this short test (`1202 ms` vs `1098 ms`), so this confirms functional regression safety but not throughput parity.
- The 1M "QAT mode" run did not use QAT because the current default profile recordsize is 128K and the effective max buffer is 128K. That row is a profile-policy fallback-to-software check, not a 1M QAT offload result.
- Compared with the 2026-05-18 balanced 128K profile run, this DKMS run is in the same performance band: QAT 128K elapsed time was `1202 ms` here versus `1157 ms` previously, and QAT service time per MiB improved from about `10.99 ms/MiB` to `9.72 ms/MiB`. The elapsed delta is small enough that it should be treated as benchmark noise unless reproduced in a longer matrix.

## Validation conclusion

The DKMS deployment is acceptable for continued development. It preserves QAT functionality, keeps ZFS and QAT in lock-step through `/usr/src/qat-4.28.0-00004`, and does not show a gross performance regression. It does not change the standing performance conclusion: QAT 1.x still saves CPU, but 128K latency/throughput remains worse than software gzip in this workload.
