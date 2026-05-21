# QAT DC Polling Experiment - 2026-05-19

## Purpose

Test QAT 1.x kernel DC poll delivery for OpenZFS gzip compression. The goal was
to determine whether avoiding interrupt delivery reduces QAT response latency
without stranding requests.

## Implementation

Added ZFS module parameters:

```text
zfs_qat_dc_poll=profile|0|1
zfs_qat_dc_poll_interval_us=profile|integer
zfs_qat_dc_poll_quota=profile|integer
```

Defaults:

```text
zfs_qat_dc_poll=profile             # effective 0
zfs_qat_dc_poll_interval_us=profile # effective 0
zfs_qat_dc_poll_quota=profile       # effective 0, QAT polls all responses
```

When polling is enabled, ZFS starts one kernel poller thread named
`zfs_qat_dc_poll`. The thread polls all initialized QAT DC instances while ZFS
has QAT DC compression or decompression requests in flight. Synchronous callers
sleep on their normal completion object; callbacks still complete the request.

Polling is a permanent operator-visible option, but it is intentionally fail
closed. During QAT DC init, ZFS queries every selected DC instance with
`cpaDcInstanceGetInfo2()` and verifies the driver's `isPolled` state matches
the effective `zfs_qat_dc_poll` value. A mismatch disables QAT DC init instead
of allowing requests to be submitted into a delivery mode ZFS is not servicing.
This protects both unsafe directions:

- `DcNIsPolled = 1` with `zfs_qat_dc_poll=0|profile`
- `DcNIsPolled = 0` with `zfs_qat_dc_poll=1`

Because the QAT driver delivery mode comes from the device config and ZFS starts
the poller only at QAT DC init, switch polling mode by changing both the QAT
config and the ZFS module/boot parameter, then rebuilding initramfs and
rebooting. Do not rely on changing `zfs_qat_dc_poll` live after QAT DC init.

Polling originally forced the async compression path off. As of the 2026-05-21
async-completion change, polling no longer disables async compression because
the central poller can drive async callbacks through the aggregate QAT DC
in-flight accounting. Accepted async requests now have timeout fallback and
late-completion cleanup. Quarantine still disables async as a conservative
policy choice because synchronous quarantine has separate ownership semantics.

Added QAT kstats:

```text
dc_poll_calls
dc_poll_success
dc_poll_retries
dc_poll_fails
dc_poll_ns
```

The benchmark harness now records the polling module parameters and these
counters.

## Failed Shapes

Config-only polling is unsafe. Setting `DcNIsPolled = 1` in the QAT config
without ZFS calling `icp_sal_DcPollInstance()` can leave QAT requests without
response delivery.

Per-request waiter polling was also unsafe. The first ZFS implementation had
each waiting thread poll the request's submitting instance. A 128K smoke run
stranded requests and left ZFS writes blocked. A follow-up implementation that
had each waiter poll all instances also stranded requests, likely because the
QAT traditional poll API is not reentrant enough for many concurrent waiters
polling the same instances. Both failed states required forced reboot on the
test host.

The working shape is one central poller thread.

## Test Setup

- Host: `pve.drewnet.online`
- Cards: 2x `dh895xcc`
- Kernel: `7.0.0-3-pve`
- QAT driver: DKMS `qat/4.28.0-00004`
- ZFS module srcversion: `68DB72821FC4E22B23E9452`
- Benchmark source:
  `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`
- Source size: `191,346,108` bytes
- Harness: `/root/qat-phase4-benchmark.sh`
- Command shape:
  `ITERS=3 RECORDS="128K" MODES="qat sw" VERIFY_MODE=sw`
- Temporary QAT config:
  `[KERNEL_QAT] Dc0IsPolled` through `Dc5IsPolled` set to `1` on both cards
- Temporary ZFS boot params:
  `zfs_qat_dc_poll=1 zfs_qat_dc_poll_interval_us=0 zfs_qat_dc_poll_quota=0`

The host was restored after the matrix:

```text
zfs_qat_dc_poll=profile
[KERNEL_QAT] Dc0IsPolled through Dc5IsPolled = 0
```

## Artifacts

```text
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-smoke-128k-jobs1-poller-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-poller-default128k-jobs1-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-poller-default128k-jobs4-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-poll-poller-default128k-jobs8-20260519.csv
.codex/skills/openzfs-qat/artifacts/zfs-qat-irq-smoke-after-poller-128k-jobs1-20260519.csv
```

## Result Summary

`qat_vs_sw_pct` is elapsed time versus software in the same polling-config run.
Negative is faster than software.

| Jobs | QAT avg ms | SW avg ms | QAT vs SW | QAT MiB/s | SW MiB/s | QAT CPU active | SW CPU active | Poll calls | Poll success | Poll retries |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 766.154 | 795.120 | -3.64% | 238.44 | 229.84 | 1.87% | 3.81% | 246,992 | 1,271 | 245,721 |
| 4 | 1094.984 | 1073.958 | +1.96% | 667.14 | 680.41 | 5.43% | 13.51% | 821,916 | 5,705 | 816,211 |
| 8 | 1671.862 | 1665.943 | +0.36% | 875.25 | 877.26 | 9.14% | 21.08% | 1,891,772 | 11,007 | 1,880,765 |

Driver timing under polling:

| Jobs | Avg driver wait | ZFS wait per MiB | CPU active s/GiB |
|---:|---:|---:|---:|
| 1 | 755.9 us | 6.12 ms/MiB | 5.15 |
| 4 | 1333.5 us | 10.76 ms/MiB | 5.33 |
| 8 | 1442.8 us | 11.66 ms/MiB | 6.86 |

## Interpretation

- Central polling works functionally for synchronous QAT compression.
- Polling did not materially improve the tested 128K concurrent cases.
- Polling produced a useful single-job result in this run: QAT was 3.6% faster
  than software while using about half the active CPU.
- At `JOBS=4` and `JOBS=8`, polling was near parity on elapsed time but still
  slower than software by 2.0% and 0.4%, while using much less active CPU.
- Poll retry counts are very high with interval `0`, so busy polling is
  expensive in polling operations even though total poll time was modest.

## Next Target

The interval sweep was completed in
`qat-dc-poll-interval-sweep-20260519.md`.

Current result: polling is safe enough to keep as a permanent operator-visible
option with the init-time mode validation. The best broad interval from the
128K sweep was `10 us`, but polling is not a universal elapsed-time win. QAT
still provides large active-CPU reductions and can handle `256K`, `512K`, and
`1M` records when `zfs_qat_dc_profile_recordsize=1048576` makes the effective
QAT max buffer large enough.
