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
#define QAT_DC_MAX_BUF_SIZE QAT_MAX_BUF_SIZE
```

`module/os/linux/zfs/qat_compress.c` now uses the compression-specific bounds in `qat_dc_use_accel()` and intermediate buffer sizing.

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

## Follow-Up

- Continue phase 4 with throughput and latency as first-class requirements. Future benchmark output should include throughput, p50/p95/p99/max latency, CPU cost, compression ratio, QAT kstats, and failure counters.
- Investigate larger-record QAT compression explicitly. Today, records above `128 KiB` fall back to software because of the OpenZFS QAT implementation threshold; before raising that threshold, prove QAT 1.x behavior with compressible and incompressible data, correctness checks, and overflow/failure counters.
- Expose `zfs_qat_dc_max_instances` and `zfs_qat_cy_max_instances` as init-time module-parameter caps with default `48`, preserving current behavior while making the cap explicit for testing. Document that operators normally should not need to change these values.
- Evaluate optimization bias parameters only after measurements identify real policy choices. A throughput/latency bias such as `latency`, `balanced`, and `throughput` is useful if queueing, batching, thresholds, or reuse strategies create measured tradeoffs. A performance/ratio bias such as `performance`, `balanced`, and `compressionratio` is useful if compression effort or fallback policy creates measured tradeoffs.
- Keep explicit low-level parameters for benchmarking first. Bias parameters should later set coherent defaults across those low-level knobs; they should not be added as no-op labels before the policies are proven.
- Park NUMA performance tuning until a true multi-socket QAT 1.x host is available.
