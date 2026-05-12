# Phase 4 Results

Run date: 2026-05-12.

Scope:

- Re-evaluate QAT compression offload eligibility for QAT 1.x.
- Measure QAT/software behavior across ZFS record sizes.
- Review instance caps, allocation costs, failure counters, and NUMA constraints.

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
```

Host DKMS source backup created before installing the phase 4 patch:

```text
/root/zfs-2.4.99.pre-phase4.20260512T215440Z
/root/zfs-2.4.99.pre-phase4.latest -> /root/zfs-2.4.99.pre-phase4.20260512T215440Z
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
- The fixed `QAT_DC_MAX_INSTANCES = 48` cap is harmless for this host because the active QAT configuration exposes only two DC instances.
- The fixed `QAT_CRYPT_MAX_INSTANCES = 48` cap was reviewed but not changed because phase 4 did not benchmark crypto/checksum.
- `qat_compress_impl()` still allocates and maps buffer-list metadata per request. This is a likely contributor to QAT not beating software gzip in elapsed time on this small single-stream matrix, but this pass did not make an allocation-cache change because that needs a separate correctness review.
- NUMA remains a benchmark constraint: the QAT device is on node `2`, while boot logs show remote-node QAT access from other application nodes.

## Code Changes

`include/sys/qat.h` now defines:

```c
#define QAT_DC_MIN_BUF_SIZE (8*1024)
#define QAT_DC_MAX_BUF_SIZE QAT_MAX_BUF_SIZE
```

`module/os/linux/zfs/qat_compress.c` now uses the compression-specific bounds in `qat_dc_use_accel()` and intermediate buffer sizing.

The patch was built and installed on `pve.drewnet.online` with DKMS, followed by `update-initramfs -u -k 7.0.0-3-pve` and reboot. No dracut package operation was performed.

Loaded module after reboot:

```text
zfs-kmod-2.4.99-1
filename: /lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko
srcversion: E2AA354E54E58F2FA7A7545
depends: spl,qat_api
```

Post-patch threshold validation:

```text
recordsize comp_requests comp_in comp_out dc_fails compressratio used  sha_ok
4K         0             0       0        0        5.57x         45.0M yes
8K         23358         191348736 6279246 0       4.43x         55.3M yes
```

This confirms 4 KiB gzip records now use software gzip without QAT failures, while 8 KiB records still use QAT compression successfully.

## Follow-Up

- If QAT compression throughput remains important, next work should focus on reducing per-request allocation and mapping costs in `qat_compress_impl()`.
- If larger-record QAT compression is considered later, test it explicitly against software gzip with correctness checks and QAT failure counters before raising `QAT_DC_MAX_BUF_SIZE`.
- NUMA-aware benchmarking should pin workload generation and inspect where ZFS compression work actually runs before drawing broad throughput conclusions.
