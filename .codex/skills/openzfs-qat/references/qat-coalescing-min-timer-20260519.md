# QAT Interrupt Coalescing Minimum-Timer Test - 2026-05-19

## Purpose

Test whether reducing QAT DC bank interrupt coalescing latency improves the
driver response-wait cost observed in the QAT driver timing matrix.

This is a QAT runtime/configuration test, not a ZFS code change.

## Source Findings

- QAT 4.28 kernel DC instances support interrupt and poll response delivery.
- `DcNIsPolled = 0` creates interrupt-delivered RX rings.
- `DcNIsPolled = 1` creates poll-delivered RX rings.
- Poll delivery requires a caller to poll the DC instance with
  `icp_sal_DcPollInstance()`.
- The OpenZFS tree does not currently call `icp_sal_DcPollInstance()`, so
  switching `[KERNEL_QAT]` DC instances to poll mode is not safe as a config-only
  experiment.
- The QAT transport driver reads `InterruptCoalescingEnabled` and
  `InterruptCoalescingTimerNs` from generated `Accelerator0` bank config.
- If coalescing is disabled in config, the driver still uses the optimized
  coalescing register path but sets the bank timer to the minimum allowed value.

Relevant source paths:

```text
contrib/qat/QAT.L.4.28.0-00004/quickassist/lookaside/access_layer/src/common/ctrl/sal_compression.c
contrib/qat/QAT.L.4.28.0-00004/quickassist/lookaside/access_layer/src/qat_kernel/src/qat_transport.c
contrib/qat/QAT.L.4.28.0-00004/quickassist/qat/drivers/crypto/qat/qat_common/adf_transport.c
```

## Host Setup

- Host: `pve.drewnet.online`
- Cards: 2x `dh895xcc`
- Kernel: `7.0.0-3-pve`
- QAT driver: DKMS `qat/4.28.0-00004`
- ZFS module: DKMS `zfs/2.4.99`
- Benchmark source:
  `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`
- Source size: `191,346,108` bytes
- Harness: `/root/qat-phase4-benchmark.sh`
- Command shape:
  `ITERS=3 RECORDS="128K" MODES="qat sw" VERIFY_MODE=sw`
- ZFS profile recordsize: default `131072`

## Config Tested

Temporary global QAT config added to both `/etc/dh895xcc_dev0.conf` and
`/etc/dh895xcc_dev1.conf`:

```text
InterruptCoalescingEnabled = 0
InterruptCoalescingTimerNs = 10000
InterruptCoalescingNumResponses = 0
```

After reboot, QAT debug config reported active DC banks with:

```text
BankNInterruptCoalescingEnabled = 0
BankNInterruptCoalescingTimerNs = 10000
BankNInterruptCoalescingNumResponses = 0
```

The host was restored after the benchmark. Active DC banks now again report:

```text
BankNInterruptCoalescingEnabled = 1
BankNInterruptCoalescingTimerNs = 10000
BankNInterruptCoalescingNumResponses = 0
```

## Artifacts

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-coalescing-baseline-default128k-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-coalescing-baseline-default128k-jobs4-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-coalescing-baseline-default128k-jobs8-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-coalescing-min-default128k-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-coalescing-min-default128k-jobs4-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-coalescing-min-default128k-jobs8-20260519.csv
```

## Results

`min-timer delta` compares the temporary minimum-timer config against the
same-day default coalescing baseline. Negative elapsed and wait deltas are
better.

| Jobs | Default QAT avg ms | Min-timer QAT avg ms | Min-timer elapsed delta | Default driver wait | Min-timer driver wait | Driver wait delta | CPU active delta |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 789.221 | 759.685 | -3.74% | 754.3 us | 742.6 us | -1.55% | +0.15 pp |
| 4 | 1090.737 | 1095.720 | +0.46% | 1331.1 us | 1240.6 us | -6.80% | -0.23 pp |
| 8 | 1549.880 | 1544.931 | -0.32% | 1302.8 us | 1300.9 us | -0.14% | +0.87 pp |

Software rows moved between the two reboots as well:

| Jobs | Default SW avg ms | Min-timer SW avg ms | SW elapsed delta |
|---:|---:|---:|---:|
| 1 | 751.114 | 737.232 | -1.85% |
| 4 | 1091.938 | 1071.472 | -1.87% |
| 8 | 1491.629 | 1451.144 | -2.71% |

## Interpretation

- Minimum-timer coalescing did not produce a material end-to-end QAT win.
- Driver response wait improved at `JOBS=4`, but elapsed time slightly
  regressed, so the improvement did not translate into application-level
  throughput.
- `JOBS=8` was effectively flat on elapsed time and driver wait, with higher
  active CPU.
- Because software rows also moved by 1.9-2.7% between reboots, the small
  single-job QAT elapsed improvement is not strong enough to treat as a
  configuration win.
- Keep the default QAT coalescing setting for now.

## Next Target

Do not continue with config-only polling until ZFS has a real poller design.
The next QAT-side target should be one of:

- Add a guarded experimental ZFS/QAT poller path for DC instances configured
  with `DcNIsPolled = 1`, then compare polling interval versus latency, CPU,
  and throughput.
- Inspect QAT request distribution across cards, banks, and instances to verify
  load balancing is even at `JOBS=1`, `JOBS=4`, and `JOBS=8`.
- Instrument per-instance driver timing or completion counts so aggregate
  response wait can be tied to specific QAT endpoints instead of only the
  global `qat_api.ko` totals.
