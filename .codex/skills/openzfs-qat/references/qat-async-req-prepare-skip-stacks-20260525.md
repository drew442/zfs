# QAT Async Request Prepare Stack-Clear Reduction - 2026-05-25

## Change

The async QAT compression path already reuses one `qat_dc_async_t` request object per buffer slot. The remaining per-request prepare cost still included clearing the entire request object, including embedded stack page-array storage that is overwritten before use.

`qat_dc_async_req_prepare()` now clears request state before and after the embedded stack arrays, but skips clearing the embedded stack arrays themselves:

- `in_pages_stack`
- `out_pages_stack`
- `scratch_pages_stack`

The published page counts are reset before use, and buffer-list construction overwrites the stack-array entries before those counts are exposed to cleanup. This keeps stale array entries unreachable while avoiding unnecessary per-request memory writes.

## Validation

Local checks:

- `git diff --check`: pass
- `bash -n .codex/skills/openzfs-qat/scripts/qat-phase4-benchmark.sh`: pass

Host deployment:

- Synced `module/os/linux/zfs/qat_compress.c` to `pve.drewnet.online:/usr/src/zfs-2.4.99/`
- Rebuilt and installed ZFS DKMS against `/usr/src/qat-4.28.0-00004`
- Ran `depmod -a` and `update-initramfs -u -k 7.0.0-3-pve`
- Rebooted into `7.0.0-3-pve`
- Verified `qat.service` active, pools healthy, and no QAT failures

## Benchmark

Artifacts:

- `artifacts/async-req-prepare-skip-stacks-20260525/zfs-qat-async-req-prepare-skip-stacks-1m-jobs4-20260525.csv`
- `artifacts/async-req-prepare-skip-stacks-20260525/zfs-qat-async-req-prepare-skip-stacks-1m-jobs8-20260525.csv`
- `artifacts/async-req-prepare-skip-stacks-20260525/summary.csv`

Benchmark shape:

- Pool: `nvme_scratch`
- Source: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`
- Record size: `1M`
- Jobs: `4`, `8`
- Iterations: `3`
- Modes: `qat`, `sw`
- Verify mode: `sw`
- Temporary boot profile: `zfs_qat_dc_profile=throughput`, `zfs_qat_dc_profile_recordsize=1048576`, `zfs_qat_dc_async=1`, `zfs_qat_dc_async_max_inflight=96`, `zfs_qat_dc_async_cap_policy=throughput`

## Results

| Change | Mode | Jobs | Mean elapsed ms | Mean write MiB/s | Mean CPU active s/GiB | QAT byte share | Fallback/cap skip share | Request prepare ns/request |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| async request slot reuse | qat | 4 | 744.619 | 982.143 | 5.309 | 79.823% | 20.400% | 1102.313 |
| skip stack clear | qat | 4 | 740.430 | 986.873 | 5.685 | 75.807% | 24.410% | 921.894 |
| async request slot reuse | sw | 4 | 757.582 | 963.570 | 11.049 | 0.000% | n/a | n/a |
| skip stack clear | sw | 4 | 758.835 | 963.457 | 11.187 | 0.000% | n/a | n/a |
| async request slot reuse | qat | 8 | 944.200 | 1546.747 | 6.070 | 80.143% | 20.480% | 1138.370 |
| skip stack clear | qat | 8 | 1022.407 | 1428.550 | 6.467 | 80.190% | 20.400% | 948.022 |
| async request slot reuse | sw | 8 | 897.447 | 1629.887 | 12.655 | 0.000% | n/a | n/a |
| skip stack clear | sw | 8 | 961.698 | 1522.767 | 13.784 | 0.000% | n/a | n/a |

## Interpretation

This change reduced `qat_req_alloc_ns_per_req` by about 16-17% in the 1M QAT runs:

- jobs4: `1102.313` to `921.894` ns/request
- jobs8: `1138.370` to `948.022` ns/request

The end-to-end elapsed result is mixed:

- jobs4 QAT improved slightly and remained faster than software for this run.
- jobs8 QAT regressed versus the previous QAT run and remained slower than software.
- The jobs8 software control also regressed by about 7%, so elapsed-time movement in this small run should be treated as noisy.

The important result for request-overhead work is that request prepare cost moved in the intended direction without changing request shape, compression ratio, or correctness. It is not enough to materially change the larger result because QAT service time is still dominated by wait time rather than local request preparation.

## Current Status

Keep this optimization unless a later stress test exposes a stale-entry cleanup bug. It is a narrow CPU-side request-overhead reduction and does not change policy or hardware behavior.

The next request-overhead targets should focus on work that can reduce request count, driver wait exposure, or fallback/cap-skip churn. Pure local nanosecond-scale prepare reductions are now unlikely to produce large end-to-end movement by themselves.
