# QAT Async Request Slot Reuse - 2026-05-25

## Goal

Resume request-overhead reduction after synchronous request reuse by removing
per-request async request-object allocation/free when an exclusive QAT buffer
slot is available.

The target is `qat_dc_async_t` in the asynchronous QAT compression path. Before
this change, async compression reused buffer-list metadata, scratch storage, and
page arrays from the per-instance buffer slot, but still allocated and freed the
`qat_dc_async_t` request object for every accepted async request.

## Code Change

`module/os/linux/zfs/qat_compress.c` now lets each buffer slot retain a reusable
async request object:

- `qat_dc_buffer_slot_t` has an `async_req` pointer.
- `qat_dc_buffer_slot_async_req()` lazily allocates and prepares a slot-owned
  `qat_dc_async_t`.
- `qat_dc_async_req_prepare()` centralizes async request initialization for
  slot-owned and heap-owned async requests.
- `qat_dc_async_t` has a `from_slot` marker so cleanup releases slot-owned
  requests back to the slot instead of freeing them.
- Async request allocation now happens after buffer-slot acquisition, so buffer
  slot hits can avoid per-request heap allocation/free.
- The existing heap allocation path remains for buffer-slot misses.

Cleanup ordering was tightened:

- Async cleanup now releases the buffer slot only after it has finished reading
  request fields and freeing non-slot resources.
- Retained sync-request cleanup now snapshots slot/from-slot state before
  releasing the slot, avoiding reads from a slot-owned request after the slot
  becomes reusable.

Observability uses the existing `dc_compress_req_slot` counter. It now counts
both sync and async compression requests that used a slot-owned request object.

## Validation

Local checks:

- `git diff --check`
- `bash -n .codex/skills/openzfs-qat/scripts/qat-phase4-benchmark.sh`

Host checks on `pve.drewnet.online`:

- Synced source to `/usr/src/zfs-2.4.99`.
- Regenerated DKMS build files with `./autogen.sh` and `./scripts/dkms.mkconf`.
- Forced ZFS DKMS rebuild with `ICP_ROOT=/usr/src/qat-4.28.0-00004`.
- Installed the rebuilt module, ran `depmod`, updated initramfs, and rebooted.
- Verified the host booted `7.0.0-3-pve`.
- Verified loaded `zfs.ko` srcversion: `D57CD070222121EB8D3F52B`.
- Verified `zpool status -x`: all pools healthy.
- Verified QAT DC remained enabled with `dc_instances=12`, `dc_fails=0`, and
  `dc_watchdog_health=1`.

## Smoke Benchmark

Temporary runtime settings:

- `zfs_qat_dc_async=1`
- `zfs_qat_dc_async_max_inflight=96`
- `zfs_qat_dc_async_cap_policy=throughput`

The host was restored afterward to:

- `zfs_qat_dc_async=profile`
- `zfs_qat_dc_async_max_inflight=profile`
- `zfs_qat_dc_async_cap_policy=profile`
- `zfs_qat_dc_profile=balanced`
- `zfs_qat_dc_profile_recordsize=131072`
- `zfs_qat_dc_max_buf_size=profile`

Benchmark shape:

- Source: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`
- Pool/root: `nvme_scratch/bench`
- Records: `128K` plus automatic `1M` control row
- Jobs: `4`
- Iterations: `1`
- Modes: `qat`
- Verify mode: `sw`

Artifacts:

- `artifacts/async-req-slot-20260525/zfs-qat-async-req-slot-smoke-20260525.csv`
- `artifacts/async-req-slot-20260525/summary.csv`

## Results

| record | jobs | elapsed ms | QAT byte share | fallback share | async submits | async completions | request slot uses | request alloc ns/request | request free ns/request | dc fails | SHA |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 128K | 4 | 792.271 | 72.16% | 27.84% | 5840 | 4214 | 2599 | 1412.401 | 263.626 | 0 | yes |
| 1M | 4 | 723.637 | 0.00% | na | 0 | 0 | 0 | na | na | 0 | yes |

The `128K` row exercised the async QAT path and shows slot-owned request reuse
on buffer-slot hits. The `1M` row is a control row from the benchmark harness'
required 1M inclusion; it fell back to software because `zfs_qat_dc_max_buf_size`
was still the balanced-profile 128K value and cannot be raised after QAT DC
initialization.

## Interpretation

This is a structural request-overhead reduction for async QAT requests:

- Accepted async requests that acquire a buffer slot no longer need a heap
  allocate/free for `qat_dc_async_t`.
- Buffer-slot misses still use the existing heap path, preserving behavior under
  pressure.
- The smoke result confirms correctness and that slot-owned async requests are
  used in practice.

This smoke is not a performance conclusion. It validates the implementation and
counter behavior only. A full before/after benchmark should be run with a boot
profile that makes 1M QAT-eligible if comparing large-record performance.

## 1M Benchmark

The host was later rebooted into a temporary 1M QAT profile so the large-record
row exercised QAT instead of falling back to software:

- `zfs_qat_dc_profile=throughput`
- `zfs_qat_dc_profile_recordsize=1048576`
- `zfs_qat_dc_async=1`
- `zfs_qat_dc_async_max_inflight=96`
- `zfs_qat_dc_async_cap_policy=throughput`

Artifacts:

- `artifacts/async-req-slot-1m-20260525/zfs-qat-async-req-slot-1m-jobs4-20260525.csv`
- `artifacts/async-req-slot-1m-20260525/zfs-qat-async-req-slot-1m-jobs8-20260525.csv`
- `artifacts/async-req-slot-1m-20260525/summary.csv`

Benchmark shape:

- Source: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`
- Pool/root: `nvme_scratch/bench`
- Record size: `1M`
- Jobs: `4`, `8`
- Iterations: `3`
- Modes: `qat sw`
- Verify mode: `sw`

Results:

| jobs | mode | median elapsed ms | mean elapsed ms | mean CPU s/GiB | mean QAT byte share | mean fallback share | mean async completions | mean request slot uses | request free ns/request | SHA | dc fails |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|
| 4 | qat | 737.186 | 744.619 | 5.309 | 79.823% | 20.400% | 582.667 | 582.667 | 0.000 | yes | 0 |
| 4 | sw | 754.459 | 757.582 | 11.049 | 0.000% | na | 0.000 | 0.000 | na | yes | 0 |
| 8 | qat | 938.863 | 944.200 | 6.070 | 80.143% | 20.480% | 1170.000 | 1170.000 | 0.000 | yes | 0 |
| 8 | sw | 921.602 | 897.447 | 12.655 | 0.000% | na | 0.000 | 0.000 | na | yes | 0 |

The 1M result is mixed and useful:

- At `jobs=4`, QAT was slightly faster than same-window software on both median
  and mean elapsed time, while using roughly half the active CPU seconds per GiB.
- At `jobs=8`, QAT was slower than same-window software on median and mean
  elapsed time, but still used roughly half the active CPU seconds per GiB.
- In all QAT rows, accepted async completions used slot-owned request objects
  and request free cost was eliminated.
- QAT byte share remained around 80%, so these were hybrid async rows with
  software fallback still contributing about 20% of source bytes.

After the benchmark, `/etc/modprobe.d/zfs-qat.conf` was restored to the default
balanced profile config, initramfs was regenerated, the host was rebooted, and
the restored state was verified:

- `zfs_qat_dc_profile=balanced`
- `zfs_qat_dc_profile_recordsize=131072`
- `zfs_qat_dc_max_buf_size=profile`
- `zfs_qat_dc_async=profile`
- `zfs_qat_dc_async_max_inflight=profile`
- `zfs_qat_dc_async_cap_policy=profile`
- `dc_fails=0`
- `dc_watchdog_health=1`
- `zpool status -x`: all pools healthy
