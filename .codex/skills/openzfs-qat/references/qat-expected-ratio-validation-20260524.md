# QAT Expected-Ratio Profile Validation - 2026-05-24

Purpose: validate the new `zfs_qat_dc_expected_ratio` profile input before
returning to request-overhead reduction.

## Configuration

Host: `pve.drewnet.online`

```text
kernel: 7.0.0-3-pve
QAT devices: 2x dh895xcc
QAT DC instances: 12
profile under test: zfs_qat_dc_profile=balanced
ratio profile under test: zfs_qat_dc_ratio_profile=balanced
profile record size under test: zfs_qat_dc_profile_recordsize=1048576
records: 128K, 512K, 1M
jobs: 1, 4, 8
iterations: 2
modes: qat, sw
verify mode: sw
media roots: nvme_scratch/bench, test-hdd-pool/bench
sources: random, mixed random/zero, TIFF
```

Each expected-ratio value was applied through `/etc/modprobe.d/zfs-qat.conf`,
`update-initramfs`, and reboot so session-global QAT DC settings were active
from initialization. The host was restored afterward to the normal default
profile state:

```text
zfs_qat_dc_expected_ratio=unknown
zfs_qat_dc_profile_recordsize=131072
```

All pools were healthy after the run.

## Artifacts

```text
.codex/skills/openzfs-qat/artifacts/expected-ratio-matrix-20260524/
.codex/skills/openzfs-qat/artifacts/expected-ratio-matrix-20260524/expected-ratio-raw-summary-20260524.csv
.codex/skills/openzfs-qat/artifacts/expected-ratio-matrix-20260524/expected-ratio-comparison-20260524.csv
.codex/skills/openzfs-qat/artifacts/expected-ratio-matrix-20260524/expected-ratio-aggregate-20260524.csv
```

The directory contains 72 raw benchmark CSVs. The generated comparison summary
contains 216 QAT-vs-software comparison rows. All QAT rows had `sha_ok=yes` and
total `dc_fails_delta=0`.

Note: the raw CSV files from this run were generated before fixing a harness
row-alignment bug. Raw rows are one column shorter than the header because
`zfs_qat_dc_expected_ratio` was present in the header but missing from raw row
emission. The affected columns are after the profile fields. The summaries in
this note use only pre-shift raw columns plus filename metadata, and the harness
has been fixed for future runs.

## Aggregate Results

Negative `QAT vs SW` means the QAT-labelled policy completed faster than
software gzip. Negative CPU means the QAT-labelled policy used less active CPU
time per GiB than software gzip.

| Expected ratio | Media | Source | Mean QAT vs SW | Mean CPU delta | Mean QAT byte share |
|---|---|---|---:|---:|---:|
| unknown | NVMe | random | -43.2% | -96.0% | 100.1% |
| low | NVMe | random | -31.1% | -62.6% | 66.7% |
| medium | NVMe | random | -14.5% | -31.1% | 33.4% |
| high | NVMe | random | 15.1% | -93.1% | 100.1% |
| unknown | NVMe | mixed | -7.4% | -85.9% | 100.1% |
| low | NVMe | mixed | -3.2% | -56.9% | 66.7% |
| medium | NVMe | mixed | -3.8% | -28.9% | 33.4% |
| high | NVMe | mixed | 20.9% | -81.6% | 100.1% |
| unknown | NVMe | TIFF | 6.4% | -64.1% | 100.2% |
| low | NVMe | TIFF | 1.2% | -48.3% | 66.8% |
| medium | NVMe | TIFF | 0.7% | -23.7% | 33.4% |
| high | NVMe | TIFF | 7.5% | -65.1% | 100.2% |
| unknown | HDD | random | -1.0% | -91.6% | 100.1% |
| low | HDD | random | -1.3% | -62.0% | 66.7% |
| medium | HDD | random | 0.8% | -29.6% | 33.4% |
| high | HDD | random | 0.0% | -91.4% | 100.1% |
| unknown | HDD | mixed | 2.0% | -81.3% | 100.1% |
| low | HDD | mixed | 5.5% | -53.2% | 66.7% |
| medium | HDD | mixed | 1.0% | -28.2% | 33.4% |
| high | HDD | mixed | 7.7% | -80.4% | 100.1% |
| unknown | HDD | TIFF | 3.7% | -61.8% | 100.2% |
| low | HDD | TIFF | 3.3% | -42.7% | 66.9% |
| medium | HDD | TIFF | 1.1% | -24.5% | 33.4% |
| high | HDD | TIFF | 4.4% | -62.6% | 100.2% |

## Decisions

- Keep `unknown` as the safe default. It remains the strongest elapsed-time
  choice for random and mixed NVMe in this balanced-profile test, and it keeps
  full QAT byte share where records are eligible.
- Keep the `low` mapping as an opt-in low-ratio policy. It reduces QAT byte
  share by falling back for `128K`, uses static Huffman for eligible larger
  records, and still preserves useful CPU reductions. It is not a better
  general default than `unknown`.
- Keep the `medium` mapping as a conservative moderate-ratio policy. It caps
  profile-managed QAT records at `512K`, reduces QAT byte share to about
  one-third in this matrix, and improved or nearly matched elapsed time in the
  HDD mixed/TIFF cases where the earlier moderate-ratio concern came from.
- Reject the automatic `high -> QAT level 4` mapping. It regressed elapsed time
  broadly, including `+15.1%` on NVMe random, `+20.9%` on NVMe mixed, and
  `+7.7%` on HDD mixed. The implementation now leaves `high` at balanced
  compression effort; operators should use `zfs_qat_dc_ratio_profile=ratio`
  when level 4 is explicitly desired.
- Do not add storage-media profile flags yet. These results still show that
  media class alone is not a reliable selector; source compressibility and
  request overhead dominate the decision.

## Next Step

Resume request-overhead reduction. The profile validation did not identify a
new profile mapping that is more important than continuing to reduce per-request
QAT overhead.
