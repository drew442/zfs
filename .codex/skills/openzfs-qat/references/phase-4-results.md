# Phase 4 Results

Run date: 2026-05-12.

Scope:

- Re-evaluate QAT compression offload eligibility for QAT 1.x.
- Measure QAT/software behavior across ZFS record sizes.
- Review instance caps, allocation costs, and failure counters.

## Source Baseline

Branch:

```text
qat-usability-performance
```

Starting commit:

```text
87f8adb06d9ff3df4500b2245510f3d969898647e
```

Host:

```text
pve.drewnet.online
kernel: 7.0.0-3-pve
QAT: dh895xcc, qat_dev0, NUMA node 2
zfs_qat_cpa_dc_level=4
```

Raw benchmark CSV:

```text
/root/zfs-qat-phase4-20260512-170610.csv
/root/zfs-qat-phase4-alloc-20260513-084514.csv
/root/zfs-qat-phase4-workspace-20260513-085342.csv
```

Host DKMS source backup created before installing the phase 4 patch:

```text
/root/zfs-2.4.99.pre-phase4.20260512T215440Z
/root/zfs-2.4.99.pre-phase4.latest -> /root/zfs-2.4.99.pre-phase4.20260512T215440Z
/root/zfs-2.4.99.pre-phase4-stack-final.20260512T225518Z
/root/zfs-2.4.99.pre-phase4-stack-final.latest -> /root/zfs-2.4.99.pre-phase4-stack-final.20260512T225518Z
```

## Benchmark Method

Temporary datasets were created under:

```text
test-hdd-pool/bench
```

Each run:

- Created a fresh dataset with `compression=gzip-1`, `checksum=sha256`, and the selected `recordsize`.
- Copied a source file from `nvme_scratch`.
- Captured elapsed time, aggregate CPU percentages from `/proc/stat`, compression ratio, space used, and `/proc/spl/kstat/zfs/qat` deltas.
- Verified the copied file with `cmp`.
- Destroyed the temporary dataset.

Compressible source:

```text
/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif
size: 191346108 bytes
```

Incompressible source:

```text
/nvme_scratch/source/Documents/DRKey/DRKey-2024-09-13-101316.zip.aes
size: 311392448 bytes
```

## Key Results

Compressible TIFF workload:

```text
mode recordsize elapsed_s MiB_s ratio  comp_req dc_fails
qat  4K         0.774     235.76 4.24x 46716    6071
qat  8K         0.606     301.13 4.43x 23358    0
qat  16K        0.656     278.17 7.79x 11679    0
qat  32K        0.418     436.56 6.92x 5840     0
qat  64K        0.314     581.15 11.64x 2920    0
qat  128K       0.296     616.49 17.11x 1460    0
qat  256K       0.293     622.81 21.53x 0       0
qat  1M         0.269     678.37 25.17x 0       0
sw   4K         0.798     228.67 5.57x 0        0
sw   8K         0.579     315.17 4.43x 0        0
sw   16K        0.498     366.43 7.82x 0        0
sw   32K        0.397     459.65 6.89x 0        0
sw   64K        0.268     680.90 11.55x 0       0
sw   128K       0.244     747.88 16.86x 0       0
sw   256K       0.270     675.86 21.53x 0       0
sw   1M         0.243     750.95 25.17x 0       0
```

Incompressible encrypted ZIP workload:

```text
mode recordsize elapsed_s MiB_s ratio comp_req dc_fails
qat  128K       1.323     224.46 1.00x 2376     0
sw   128K       1.215     244.42 1.00x 0        0
```

All file comparisons passed.

## Decisions

- Compression offload minimum was raised from `4 KiB` to `8 KiB`.
- The shared crypto/checksum minimum remains `4 KiB` because this phase measured compression only.
- Compression maximum remains `128 KiB`; no data from this pass justifies extending QAT compression to larger ZFS records.
- Records larger than `128 KiB` in this pass used software gzip fallback because the current OpenZFS QAT path rejects compression buffers above `QAT_DC_MAX_BUF_SIZE`; this result does not prove a QAT 1.x hardware maximum.
- The fixed `QAT_DC_MAX_INSTANCES = 48` cap is harmless for this host because the active QAT configuration exposes only two DC instances.
- The fixed `QAT_CRYPT_MAX_INSTANCES = 48` cap was reviewed but not changed because phase 4 did not benchmark crypto/checksum.
- `qat_compress_impl()` still allocates and maps buffer-list metadata per request. This is a likely contributor to QAT not beating software gzip in elapsed time on this small single-stream matrix, but this pass did not make an allocation-cache change because that needs a separate correctness review.
- NUMA remains parked for performance conclusions. `pve.drewnet.online` is a single-socket EPYC 7551P host, so its reported NUMA topology is not a suitable basis for multi-socket QAT placement policy.

## Code Changes

`include/sys/qat.h` now defines:

```c
#define QAT_DC_MIN_BUF_SIZE (8*1024)
#define QAT_DC_DEFAULT_MAX_BUF_SIZE QAT_MAX_BUF_SIZE
#define QAT_DC_ABS_MAX_BUF_SIZE (1024*1024)
```

`module/os/linux/zfs/qat_compress.c` now uses compression-specific bounds in `qat_dc_use_accel()` and intermediate buffer sizing. The effective maximum is controlled by `zfs_qat_dc_max_buf_size`, which defaults to `128 KiB`.

Follow-up allocation tuning removed three per-request heap allocations by replacing the temporary source, destination, and scratch page-pointer arrays with fixed stack arrays sized by `QAT_DC_MAX_PAGES`. The QAT API metadata buffers and `CpaBufferList` storage still use per-request allocations.

A deeper per-instance workspace experiment was built and benchmarked but abandoned. It preallocated QAT metadata/list storage under a per-instance mutex, but the host measurements regressed:

```text
variant              128K QAT elapsed_s       8K QAT elapsed_s
stack arrays only    0.338-0.442              0.591
workspace attempt    0.524-0.574              1.357
```

The workspace attempt was removed from the final patch because it serialized enough of the compression path to outweigh the saved allocations.

The patch was built and installed on `pve.drewnet.online` with DKMS, followed by `update-initramfs -u -k 7.0.0-3-pve` and reboot. No dracut package operation was performed.

Loaded module after reboot:

```text
zfs-kmod-2.4.99-1
filename: /lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko
srcversion: 5F988487C81FC03543DBBFA
depends: spl,qat_api
```

Post-patch threshold validation:

```text
recordsize comp_requests comp_in comp_out dc_fails compressratio used  sha_ok
4K         0             0       0        0        5.57x         45.0M yes
8K         23358         191348736 6279246 0       4.43x         55.3M yes
```

This confirms 4 KiB gzip records now use software gzip without QAT failures, while 8 KiB records still use QAT compression successfully.

Post-allocation-tuning validation after reinstalling the final stack-array build:

```text
recordsize comp_requests comp_in    comp_out dc_fails compressratio used  sha_ok
8K         23358         191348736 6279760  0        4.43x         55.3M yes
4K         0             0         0        0        5.57x         45.0M yes
```

## Instance-Cap Parameter Follow-Up

Run date: 2026-05-13.

The repo now exposes these init-time module-parameter caps:

```text
zfs_qat_dc_max_instances=48
zfs_qat_cy_max_instances=48
```

The implementation preserves the fixed 48-entry DC and CY arrays and treats the
new parameters as caps over the hardware-reported instance counts. Values below
`1`, values above `48`, and changes after the relevant QAT path initializes are
rejected.

Host backup before refreshing `/usr/src/zfs-2.4.99/`:

```text
/root/zfs-2.4.99.pre-instance-caps.20260513T015308Z
/root/zfs-2.4.99.pre-instance-caps.latest -> /root/zfs-2.4.99.pre-instance-caps.20260513T015308Z
```

Build and install logs:

```text
/root/zfs-qat-instance-caps-dkms-build-20260513-r2.log
/root/zfs-qat-instance-caps-dkms-install-20260513.log
/root/zfs-qat-instance-caps-initramfs-20260513.log
```

Loaded module after DKMS install, `update-initramfs -u -k 7.0.0-3-pve`, and reboot:

```text
filename: /lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko
version: 2.4.99-1
srcversion: 7E89EEFB16FBF2CA6974715
depends: spl,qat_api
```

`modinfo zfs` shows the new parameters:

```text
parm: zfs_qat_cy_max_instances:Maximum QAT crypto instances to use
parm: zfs_qat_dc_max_instances:Maximum QAT compression instances to use
```

Runtime defaults after reboot:

```text
zfs_qat_compress_disable=0
zfs_qat_cpa_dc_level=4
zfs_qat_dc_max_instances=48
zfs_qat_checksum_disable=0
zfs_qat_encrypt_disable=0
zfs_qat_cy_max_instances=48
```

After forcing the lazy init path, changing either cap from `48` to `47`
returned `Device or resource busy` and left the value unchanged.

8 KiB gzip validation after reboot:

```text
comp_requests_before=0
comp_requests_after=23358
dc_fails_before=0
dc_fails_after=0
compressratio=4.44x
used=55.3M
```

The copied file compared cleanly with `cmp`, the temporary validation dataset was
destroyed, `qat_dev0` was up, and `zpool status -x` reported all pools healthy.

## Large-Record Parameter Follow-Up

Run date: 2026-05-13.

The repo now exposes:

```text
zfs_qat_dc_max_buf_size=131072
```

The default remains `128 KiB`. Accepted values are exactly `131072`,
`262144`, `524288`, and `1048576` bytes. The value must be set before QAT DC
initializes; records larger than the configured value use software gzip
fallback.

The implementation keeps the default 128 KiB path stack-bounded and allocates
page-pointer tracking dynamically when larger records are enabled. Scratch page
tracking is heap-allocated to avoid a `-Wframe-larger-than` warning in
`qat_compress.c`.

Host backup before refreshing `/usr/src/zfs-2.4.99/`:

```text
/root/zfs-2.4.99.pre-large-records.20260513T020550Z
/root/zfs-2.4.99.pre-large-records.latest -> /root/zfs-2.4.99.pre-large-records.20260513T020550Z
```

Build and install logs:

```text
/root/zfs-qat-large-records-dkms-build-20260513-r2.log
/root/zfs-qat-large-records-dkms-install-20260513.log
/root/zfs-qat-large-records-initramfs-20260513.log
```

The host was configured for opt-in large-record validation with:

```text
options zfs zfs_qat_compress_disable=0 zfs_qat_checksum_disable=0 zfs_qat_cpa_dc_level=4 zfs_qat_dc_max_buf_size=1048576
```

Loaded module after DKMS install, `update-initramfs -u -k 7.0.0-3-pve`, and reboot:

```text
filename: /lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko
version: 2.4.99-1
srcversion: C4619ACEB8C6341E0CD1155
depends: spl,qat_api
zfs_qat_dc_max_buf_size=1048576
```

Large-record validation:

```text
source record bytes     elapsed_ms comp_req_delta dc_fail_delta ratio  used
tiff   256K   191346108 776        730            0             21.89x 9.42M
tiff   1M     191346108 680        183            0             25.45x 7.66M
zipaes 256K   311392448 2537       1188           0             1.00x  297M
zipaes 1M     311392448 2734       297            0             1.00x  297M
```

All copied files compared cleanly with `cmp`, temporary datasets were destroyed,
and `zpool status -x` reported all pools healthy. The read-side `cmp` checks
also moved decompression counters, confirming QAT decompression was used for the
large-record test data.

Post-init parameter validation:

```text
write=524288 before=1048576 rc=1 after=1048576 err=Device or resource busy
write=12345 before=1048576 rc=1 after=1048576 err=Invalid argument
```

## Benchmark Harness Follow-Up

Run date: 2026-05-13.

Added repo-tracked harness:

```text
.codex/skills/openzfs-qat/scripts/qat-phase4-benchmark.sh
```

The harness creates temporary datasets under `test-hdd-pool/bench`, toggles QAT
compression on or off per mode, writes a source file into a gzip-1 dataset,
validates the copy with `cmp`, destroys the temporary dataset, and emits CSV
rows with:

- Raw per-iteration elapsed time and throughput.
- Summary latency columns: average, p50, p95, p99, and max.
- CPU user/system/iowait/idle percentages.
- Compression ratio, used space, and logical used space.
- QAT compression/decompression kstat deltas and DC failure deltas.
- `zfs_qat_cpa_dc_level`, `zfs_qat_dc_max_buf_size`, `zfs_qat_dc_max_instances`, and loaded module `srcversion`.

Smoke test:

```text
ITERS=1 RECORDS=256K MODES=qat OUT=/root/zfs-qat-phase4-harness-smoke-20260513-r3.csv /root/qat-phase4-benchmark.sh
```

The smoke CSV had 32 columns for header, raw, and summary rows, moved QAT
compression counters for the 256 KiB TIFF workload, and completed without
leaving a `qat-phase4` temporary dataset behind.

## Follow-Up

- Continue phase 4 with throughput and latency as first-class requirements. Future benchmark output should include throughput, p50/p95/p99/max latency, CPU cost, compression ratio, QAT kstats, and failure counters.
- Continue larger-record benchmarking. Initial 256 KiB and 1 MiB validation proves QAT offload can work on this host, but the default should remain `128 KiB` until throughput and latency are compared against software gzip across the broader matrix.
- Evaluate optimization bias parameters only after measurements identify real policy choices. A throughput/latency bias such as `latency`, `balanced`, and `throughput` is useful if queueing, batching, thresholds, or reuse strategies create measured tradeoffs. A performance/ratio bias such as `performance`, `balanced`, and `compressionratio` is useful if compression effort or fallback policy creates measured tradeoffs.
- Keep explicit low-level parameters for benchmarking first. Bias parameters should later set coherent defaults across those low-level knobs; they should not be added as no-op labels before the policies are proven.
- Park NUMA performance tuning until a true multi-socket QAT 1.x host is available.
