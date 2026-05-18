# Phase 4 Results

Run date: 2026-05-12.

For human-review charts and summary tables, see
`phase-4-performance-review.md`.

Scope:

- Re-evaluate QAT compression offload eligibility for QAT 1.x.
- Measure QAT/software behavior across ZFS record sizes.
- Review instance caps, allocation costs, and failure counters.
- Instrument QAT DC latency phases before making deeper tuning changes.

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

## Latency Instrumentation Follow-Up

Run date: 2026-05-13.

The QAT DC path now exposes cumulative nanosecond kstats for these phases:

```text
dc_compress_scratch_alloc_ns
dc_compress_scratch_free_ns
dc_compress_setup_ns
dc_compress_submit_ns
dc_compress_wait_ns
dc_compress_cleanup_ns
dc_decompress_setup_ns
dc_decompress_submit_ns
dc_decompress_wait_ns
dc_decompress_cleanup_ns
```

The phase 4 benchmark harness records each value as a per-run delta. These
counters are intended to identify whether high latency is dominated by OpenZFS
wrapper overhead, scratch allocation, QAT API submission, hardware/queue wait
time, or cleanup.

Host source backup before installing the timing-kstat build:

```text
/root/zfs-2.4.99.pre-latency-kstats.20260513T093615Z
/root/zfs-2.4.99.pre-latency-kstats.latest -> /root/zfs-2.4.99.pre-latency-kstats.20260513T093615Z
```

Build and install logs:

```text
/root/zfs-qat-latency-kstats-dkms-build-20260513.log
/root/zfs-qat-latency-kstats-dkms-install-20260513.log
/root/zfs-qat-latency-kstats-initramfs-20260513.log
/root/zfs-qat-latency-kstats-dkms-build-20260513-r2.log
/root/zfs-qat-latency-kstats-dkms-install-20260513-r2.log
/root/zfs-qat-latency-kstats-initramfs-20260513-r2.log
```

Loaded module after DKMS install, `update-initramfs -u -k 7.0.0-3-pve`, and
reboot:

```text
filename: /lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko
version: 2.4.99-1
srcversion: 9005FF9E36B658359545E79
depends: spl,qat_api
```

Timing smoke CSV:

```text
/root/zfs-qat-phase4-latency-kstats-smoke-20260513.csv
/root/zfs-qat-phase4-latency-kstats-final-smoke-r3-20260513.csv
```

The smoke test used `ITERS=1 JOBS=1 RECORDS="128K" MODES="qat"`.
The copied file compared cleanly, `dc_fails=0`, and the new timing kstats
advanced from zero.

After one reboot, `qat.service` was inactive because systemd deleted the start
job to break a ZFS import ordering cycle. In that state a QAT-mode benchmark
showed `comp_requests=0` even though `adf_ctl status` reported the device up.
Starting `qat.service` explicitly and toggling `zfs_qat_compress_disable` from
`1` back to `0` restored ZFS QAT compression; the final smoke CSV above moved
QAT counters again.

Level comparison CSVs:

```text
/root/zfs-qat-phase4-level1-timing-20260513.csv
/root/zfs-qat-phase4-level4-timing-20260513.csv
```

The host was temporarily booted with `zfs_qat_cpa_dc_level=1`, benchmarked, then
returned to `zfs_qat_cpa_dc_level=4`.

```text
record level avg_ms MiB_s ratio  comp_wait_us_req decomp_wait_us_req
128K   1     859.8  212.8 16.88x 1346.1           185.4
128K   4     831.4  220.1 17.11x 1579.8           172.8
256K   1     688.9  265.4 21.63x 2679.8           317.6
256K   4     707.6  258.2 21.90x 3327.6           307.2
1M     1     586.9  311.0 25.15x 10612.6          1128.3
1M     4     617.7  295.5 25.45x 13790.5          1080.3
```

Conclusion:

- Compression wait time dominates the measured QAT path.
- Scratch allocation is not the primary latency source in these runs.
- QAT level 1 improves larger-record latency, but reduces ratio and does not
  close the software gzip gap.
- The next tuning target should be QAT DC concurrency and instance allocation,
  not scratch reuse.

## QAT DC Instance Split Follow-Up

Run date: 2026-05-13.

The host QAT driver configuration was temporarily changed from the default
`[KERNEL_QAT]` split:

```text
NumberCyInstances = 4
NumberDcInstances = 2
```

to a DC-biased split:

```text
NumberCyInstances = 2
NumberDcInstances = 4
```

Two additional DC entries were added:

```text
Dc2Name = "IPComp2"
Dc2IsPolled = 0
Dc2CoreAffinity = 7
Dc3Name = "IPComp3"
Dc3IsPolled = 0
Dc3CoreAffinity = 8
```

The total kernel QAT instance count remained six. The QAT service accepted the
config file but reported `device busy` when restarted with active ZFS QAT
handles, so the host was rebooted before benchmarking.

Source CSVs:

```text
/root/zfs-qat-phase4-dc2-jobs4-timing-20260513.csv
/root/zfs-qat-phase4-dc4-jobs4-timing-v2-20260513.csv
/root/zfs-qat-phase4-dc2-restore-smoke-20260513.csv
```

The phase 4 benchmark harness now records the active QAT driver
`qat_kernel_cy_instances` and `qat_kernel_dc_instances` columns so future CSVs
identify the driver instance split directly.

```text
record dc_instances avg_ms MiB_s comp_wait_us_req result
128K   2            1323.1 551.7 3007.8           baseline
128K   4            1354.2 539.0 3210.6           2.4% slower
256K   2            1282.8 569.1 6831.6           baseline
256K   4            1235.7 590.7 6699.4           3.7% faster
1M     2            1243.2 587.1 27455.4          baseline
1M     4            1239.1 589.2 26847.6          0.3% faster
```

Conclusion:

- The DC-biased split is not a clear win.
- The `256 KiB` workload improved modestly, but `128 KiB` regressed and `1 MiB`
  was effectively flat.
- The host was restored to the original `NumberCyInstances=4` and
  `NumberDcInstances=2` split after the test.
- The next performance target should move away from driver instance count and
  toward the synchronous wait model or separate read/write offload policy.

## QAT Decompression Policy Follow-Up

Run date: 2026-05-13.

The repo now exposes:

```text
zfs_qat_decompress_disable=0
```

This disables QAT gzip decompression independently from QAT gzip compression.
`zfs_qat_compress_disable` remains the master QAT DC disable and overrides the
decompression policy.

Host source backup before installing the decompression-policy build:

```text
/root/zfs-2.4.99.pre-decompress-policy.20260513T112711Z
/root/zfs-2.4.99.pre-decompress-policy.latest -> /root/zfs-2.4.99.pre-decompress-policy.20260513T112711Z
```

Build and install logs:

```text
/root/zfs-qat-decompress-policy-dkms-build-20260513.log
/root/zfs-qat-decompress-policy-dkms-install-20260513.log
/root/zfs-qat-decompress-policy-initramfs-20260513.log
```

Loaded module after DKMS install, `update-initramfs -u -k 7.0.0-3-pve`, and
reboot:

```text
srcversion: D872F247984AF9A2B7E42CC
parm: zfs_qat_decompress_disable:Enable/Disable QAT decompression
```

Source CSVs:

```text
/root/zfs-qat-phase4-qwrite-qread-v2-20260513.csv
/root/zfs-qat-phase4-qwrite-swread-v2-20260513.csv
/root/zfs-qat-phase4-sw-baseline-decompress-policy-v2-20260513.csv
/root/zfs-qat-phase4-qwrite-qread-jobs4-v2-20260513.csv
/root/zfs-qat-phase4-qwrite-swread-jobs4-v2-20260513.csv
/root/zfs-qat-phase4-sw-baseline-decompress-policy-jobs4-v2-20260513.csv
/root/zfs-qat-phase4-decompress-policy-harness-smoke-20260513.csv
```

The benchmark harness now supports:

```text
VERIFY_MODE=same
VERIFY_MODE=qat
VERIFY_MODE=sw
```

It also records `verify_mode` and `zfs_qat_decompress_disable` in the CSV.

Single-job summary:

```text
record qwrite_qread_ms qwrite_swread_ms sw_ms swread_vs_qread swread_vs_sw
128K   835.0           794.8            745.1 -4.8%           +6.7%
256K   687.6           709.3            667.4 +3.2%           +6.3%
1M     620.2           635.0            583.1 +2.4%           +8.9%
```

Four-job summary:

```text
record qwrite_qread_ms qwrite_swread_ms sw_ms  swread_vs_qread swread_vs_sw
128K   1335.0          1232.4           1075.3 -7.7%           +14.6%
256K   1284.8          1193.9           989.7  -7.1%           +20.6%
1M     1207.8          1183.2           921.5  -2.0%           +28.4%
```

Conclusion:

- `zfs_qat_decompress_disable=1` works as intended: QAT compression counters
  still move and QAT decompression counters stay at zero during readback.
- Software readback improves the four-job latency results relative to QAT
  readback, but the single-job result is mixed.
- Software readback does not make QAT faster than full software gzip on this
  workload.
- This should remain an operator tuning option rather than the default policy.

## QAT In-Flight Counter Follow-Up

Run date: 2026-05-13.

The repo now exposes these additional QAT DC kstats:

```text
dc_compress_inflight
dc_compress_inflight_max
dc_decompress_inflight
dc_decompress_inflight_max
```

The current counters return to zero after a run if no QAT requests are leaked.
The max counters show the peak since module load. The benchmark harness records
these fields in each raw row.

Host source backup before installing the in-flight-kstat build:

```text
/root/zfs-2.4.99.pre-inflight-kstats.20260513T120505Z
/root/zfs-2.4.99.pre-inflight-kstats.latest -> /root/zfs-2.4.99.pre-inflight-kstats.20260513T120505Z
```

Build and install logs:

```text
/root/zfs-qat-inflight-kstats-dkms-build-20260513.log
/root/zfs-qat-inflight-kstats-dkms-install-20260513.log
/root/zfs-qat-inflight-kstats-initramfs-20260513.log
```

Loaded module after DKMS install, `update-initramfs -u -k 7.0.0-3-pve`, and
reboot:

```text
srcversion: E99C3B2FDEE9BCBEBFDED14
```

Source CSVs:

```text
/root/zfs-qat-phase4-inflight-jobs1-qread-v2-20260513.csv
/root/zfs-qat-phase4-inflight-jobs1-swread-v2-20260513.csv
/root/zfs-qat-phase4-inflight-jobs4-qread-v2-20260513.csv
/root/zfs-qat-phase4-inflight-jobs4-swread-v2-20260513.csv
/root/zfs-qat-phase4-inflight-harness-smoke-20260513.csv
```

Summary:

```text
workload          record avg_ms MiB_s comp_inflight_max decomp_inflight_max
jobs1 qread      128K   855.9  213.2 25                7
jobs1 qread      256K   737.9  247.4 25                7
jobs1 qread      1M     627.3  291.3 25                7
jobs1 swread     128K   794.7  230.6 25                prior 7
jobs1 swread     256K   684.7  266.7 25                prior 7
jobs1 swread     1M     632.4  288.5 25                prior 7
jobs4 qread      128K   1313.6 555.7 50                10
jobs4 qread      256K   1303.3 560.1 50                11
jobs4 qread      1M     1262.4 578.7 50                12
jobs4 swread     128K   1243.5 587.0 50                prior 12
jobs4 swread     256K   1215.4 600.6 50                prior 12
jobs4 swread     1M     1160.6 629.2 50                prior 12
```

Conclusion:

- ZFS already drives multiple QAT DC requests in parallel.
- The high latency is not explained by QAT receiving only one synchronous
  request at a time.
- A full async ZIO rewrite may still reduce blocked worker time, but the next
  lower-risk performance target should be QAT service-time policy: Huffman mode,
  compression level, or other QAT 1.x session options.

## QAT Huffman Mode Follow-Up

Run date: 2026-05-14.

The repo now exposes:

```text
zfs_qat_cpa_dc_hufftype=dynamic
```

Accepted values are `dynamic` and `static`. The value is a global QAT
data-compression session setting and must be set before QAT DC initializes.
`dynamic` remains the default.

Host source backup before installing the Huffman-mode build:

```text
/root/zfs-2.4.99.pre-hufftype.20260514T101911Z
/root/zfs-2.4.99.pre-hufftype.latest -> /root/zfs-2.4.99.pre-hufftype.20260514T101911Z
```

Build and install logs:

```text
/root/zfs-qat-hufftype-dkms-build-20260514.log
/root/zfs-qat-hufftype-dkms-install-20260514.log
/root/zfs-qat-hufftype-initramfs-20260514.log
/root/zfs-qat-hufftype-dkms-build-20260514-r2.log
/root/zfs-qat-hufftype-dkms-install-20260514-r2.log
/root/zfs-qat-hufftype-initramfs-20260514-r2.log
/root/zfs-qat-hufftype-static-initramfs-20260514.log
/root/zfs-qat-hufftype-restore-initramfs-20260514.log
```

Loaded module after DKMS install, `update-initramfs -u -k 7.0.0-3-pve`, and
reboot:

```text
srcversion: 0A2AB5B290BEB725873A90F
```

Source CSVs:

```text
/root/zfs-qat-phase4-huff-dynamic-jobs1-swread-20260514.csv
/root/zfs-qat-phase4-huff-static-jobs1-swread-20260514.csv
/root/zfs-qat-phase4-huff-dynamic-jobs4-swread-20260514.csv
/root/zfs-qat-phase4-huff-static-jobs4-swread-20260514.csv
```

Summary:

```text
jobs huff    record avg_ms MiB_s ratio  comp_wait_ms dc_fails
1    dynamic 128K   788.0  232.1 17.11x 2313.4       0
1    static  128K   801.1  227.9 16.03x 2253.7       0
1    dynamic 256K   696.3  262.4 21.89x 2440.2       0
1    static  256K   735.8  249.4 19.53x 2362.5       0
1    dynamic 1M     609.6  299.4 25.47x 2556.9       0
1    static  1M     617.6  295.5 21.86x 2386.0       0
4    dynamic 128K   1243.4 587.3 17.16x 17459.8      0
4    static  128K   1303.1 560.2 16.07x 18663.9      0
4    dynamic 256K   1211.7 602.4 21.96x 19938.3      0
4    static  256K   1196.6 610.1 19.59x 17862.3      0
4    dynamic 1M     1158.0 630.5 25.56x 20090.3      0
4    static  1M     1162.6 628.1 21.93x 18010.4      0
```

Conclusion:

- Static Huffman works on the dh895xcc/QAT 4.28 host and did not produce DC
  failures in the tested matrix.
- Static reduced accumulated compression wait time for several larger-record
  cases, but elapsed latency and throughput were mixed.
- Static materially reduced compression ratio on the source file.
- `dynamic` remains the correct default. Static should be treated as an
  explicit tuning option and a candidate for a future performance-biased policy,
  not as a default replacement.

## QAT Compression Bound Follow-Up

Run date: 2026-05-14.

The compression path now uses `cpaDcDeflateCompressBound()` to size the
additional QAT compression scratch buffer. If the bound API fails, the code
falls back to the previous full destination-sized scratch allocation.

New QAT DC kstats:

```text
dc_compress_bound_requests
dc_compress_bound_fails
dc_compress_bound_ns
dc_compress_bound_total_bytes
dc_compress_dst_total_bytes
dc_compress_scratch_bytes
dc_compress_scratch_saved_bytes
dc_compress_overflows
dc_compress_incompressible
```

Host source backup before installing the compression-bound build:

```text
/root/zfs-2.4.99.pre-compress-bound.20260514T121246Z
/root/zfs-2.4.99.pre-compress-bound.latest -> /root/zfs-2.4.99.pre-compress-bound.20260514T121246Z
```

Build and install logs:

```text
/root/zfs-qat-compress-bound-dkms-build-20260514.log
/root/zfs-qat-compress-bound-dkms-install-20260514.log
/root/zfs-qat-compress-bound-initramfs-20260514.log
```

Loaded module after DKMS install, `update-initramfs -u -k 7.0.0-3-pve`, and
reboot:

```text
srcversion: 94BDEFB952B82D7C5DF9902
```

Source CSVs:

```text
/root/zfs-qat-phase4-bound-jobs1-swread-20260514.csv
/root/zfs-qat-phase4-bound-jobs4-swread-20260514.csv
/root/zfs-qat-phase4-bound-incompressible-20260514.csv
```

Summary versus the prior dynamic-Huffman, software-readback run:

```text
jobs record prior_ms bound_ms prior_MiB_s bound_MiB_s scratch_MB saved_MB overflow incompress
1    128K   788.0    775.0    232.1       235.7       47.9       119.5    0        0
1    256K   696.3    689.6    262.4       264.9       47.9       119.6    0        0
1    1M     609.6    622.2    299.4       293.5       48.0       119.9    0        0
4    128K   1243.4   1268.1   587.3       575.6       191.7      478.1    0        0
4    256K   1211.7   1205.2   602.4       605.8       191.5      478.2    0        0
4    1M     1158.0   1156.8   630.5       631.1       191.9      479.7    0        0
```

Incompressible 64 MiB random-source check:

```text
record avg_ms MiB_s ratio comp_requests overflow incompressible sha_ok
128K   529.3  121.2 1.00x 512           0        512            yes
1M     574.8  111.4 1.00x 64            0        64             yes
```

Conclusion:

- The bound API succeeded for all tested requests.
- The additional scratch allocation was reduced by about `71%` relative to the
  destination-sized scratch strategy.
- QAT overflow count stayed at zero, including the random-source
  incompressible test.
- Incompressible fallback behavior remained correct and read verification
  passed.
- Elapsed latency and throughput were mixed; treat this as a memory-pressure
  and observability improvement, not a direct performance fix.

## QAT Source Coalescing Follow-Up

Run date: 2026-05-14.

The repo now exposes:

```text
zfs_qat_dc_coalesce_src=0
```

The default is disabled. When enabled, the QAT compression path copies each
source record into one contiguous QAT input buffer before submission. This is a
runtime experiment; it does not require QAT DC session reinitialization.

New QAT DC kstats:

```text
dc_compress_src_buffers
dc_compress_dst_buffers
dc_compress_add_buffers
dc_compress_dst_total_buffers
dc_compress_src_buffers_max
dc_compress_dst_buffers_max
dc_compress_add_buffers_max
dc_compress_dst_total_buffers_max
dc_compress_coalesce_requests
dc_compress_coalesce_success
dc_compress_coalesce_fails
dc_compress_coalesce_bytes
dc_compress_coalesce_alloc_ns
dc_compress_coalesce_copy_ns
dc_compress_coalesce_free_ns
```

Host source backup before installing the buffer-shape/coalescing builds:

```text
/root/zfs-2.4.99.pre-buffer-shape.20260514T133038Z
/root/zfs-2.4.99.pre-buffer-shape.latest -> /root/zfs-2.4.99.pre-buffer-shape.20260514T133038Z
```

Build and install logs:

```text
/root/zfs-qat-buffer-shape-dkms-build-20260514.log
/root/zfs-qat-buffer-shape-dkms-install-20260514.log
/root/zfs-qat-buffer-shape-initramfs-20260514.log
/root/zfs-qat-src-coalesce-dkms-build-20260514.log
/root/zfs-qat-src-coalesce-dkms-install-20260514.log
/root/zfs-qat-src-coalesce-initramfs-20260514.log
/root/zfs-qat-src-coalesce-dkms-build-20260514-r2.log
/root/zfs-qat-src-coalesce-dkms-install-20260514-r2.log
/root/zfs-qat-src-coalesce-initramfs-20260514-r2.log
```

Loaded module after the final DKMS install, `update-initramfs -u -k
7.0.0-3-pve`, and reboot:

```text
srcversion: 4E6670F89115AA129322F75
```

Source CSVs:

```text
/root/zfs-qat-phase4-coalesce-off-jobs1-swread-v3-20260514.csv
/root/zfs-qat-phase4-coalesce-on-jobs1-swread-v3-20260514.csv
/root/zfs-qat-phase4-coalesce-off-jobs4-swread-v3-20260514.csv
/root/zfs-qat-phase4-coalesce-on-jobs4-swread-v3-20260514.csv
```

Summary:

```text
jobs coalesce record avg_ms MiB_s src_bufs dst_total_bufs copy_MB coalesce_ms dc_fails
1    off      128K   824.4  221.7 32       37             0.0     0.0         0
1    on       128K   749.0  244.0 1        37             191.4   44.3        0
1    off      256K   703.4  259.6 64       73             0.0     0.0         0
1    on       256K   673.9  271.0 1        73             191.4   45.1        0
1    off      1M     616.6  296.0 256      289            0.0     0.0         0
1    on       1M     630.9  289.2 1        289            191.9   62.3        0
4    off      128K   1294.3 564.0 32       37             0.0     0.0         0
4    on       128K   1289.3 566.4 1        37             765.5   173.0       0
4    off      256K   1289.0 566.6 64       73             0.0     0.0         0
4    on       256K   1203.3 606.8 1        73             765.5   169.8       0
4    off      1M     1188.7 614.3 256      289            0.0     0.0         0
4    on       1M     1217.5 604.1 1        289            767.6   232.8       0
```

Conclusion:

- The non-coalesced QAT source list is highly fragmented: 32 source buffers at
  128K, 64 at 256K, and 256 at 1M.
- Source coalescing reduced source buffers to 1 for all tested record sizes and
  had zero coalescing allocation failures.
- The copy/allocation cost was meaningful: about `44-62 ms` for the single-job
  run and `170-233 ms` for the four-job run.
- 128K and 256K improved in this matrix, while 1M regressed.
- Keep `zfs_qat_dc_coalesce_src=0` by default. It is useful as an experimental
  tuning knob and possible input to future bias policy, but not a universal
  default.

## QAT Destination Coalescing Follow-Up

Run date: 2026-05-15.

The repo now exposes:

```text
zfs_qat_dc_coalesce_dst=0
```

The default is disabled. When enabled, the QAT compression path writes to one
contiguous output buffer sized to `dst_len + add_len`, where `add_len` is the
deflate-bound scratch allowance. On successful compression, only the compressed
result is copied back to the original ZFS destination buffer. Source coalescing
was disabled during this comparison.

New QAT DC kstats:

```text
dc_compress_dst_coalesce_requests
dc_compress_dst_coalesce_success
dc_compress_dst_coalesce_fails
dc_compress_dst_coalesce_alloc_bytes
dc_compress_dst_coalesce_copy_bytes
dc_compress_dst_coalesce_alloc_ns
dc_compress_dst_coalesce_copy_ns
dc_compress_dst_coalesce_free_ns
```

Host source backup before installing the destination-coalescing build:

```text
/root/zfs-2.4.99.pre-dst-coalesce.20260515T060859Z
/root/zfs-2.4.99.pre-dst-coalesce.latest -> /root/zfs-2.4.99.pre-dst-coalesce.20260515T060859Z
```

Build and install logs:

```text
/root/zfs-qat-dst-coalesce-dkms-build-20260515.log
/root/zfs-qat-dst-coalesce-dkms-install-20260515.log
/root/zfs-qat-dst-coalesce-initramfs-20260515.log
/root/zfs-qat-dst-coalesce-dkms-build-20260515-r2.log
/root/zfs-qat-dst-coalesce-dkms-install-20260515-r2.log
/root/zfs-qat-dst-coalesce-initramfs-20260515-r2.log
```

Loaded module after the final DKMS install, `update-initramfs -u -k
7.0.0-3-pve`, and reboot:

```text
srcversion: 536449095ADB8E51B622004
```

The original `/nvme_scratch` source pool was not imported after the reboot, so
this pass used the lz4-backed copy of the same TIFF source file:

```text
/test-hdd-pool/bench/cpu-lz4/realdata-test/2021-09-05/Scanned Documents/Image.tif
```

Source CSVs:

```text
/root/zfs-qat-phase4-dst-coalesce-smoke-lz4src-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-smoke-lz4src-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-off-jobs1-r2-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-on-jobs1-r2-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-off-jobs4-r2-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-on-jobs4-r2-20260515.csv
```

Summary:

```text
jobs coalesce record avg_ms MiB_s dst_total_bufs alloc_MB copy_MB coalesce_ms dc_fails
1    off      128K   893.1  205.8 37             0.0      0.0     0.0         0
1    on       128K   863.8  211.4 1              205.4    6.5     29.6        0
1    off      256K   755.0  241.7 73             0.0      0.0     0.0         0
1    on       256K   764.8  239.3 1              205.4    6.6     29.4        0
1    off      1M     732.6  249.4 289            0.0      0.0     0.0         0
1    on       1M     733.5  249.1 1              205.9    6.7     65.0        0
4    off      128K   1896.0 385.4 37             0.0      0.0     0.0         0
4    on       128K   1884.1 387.4 1              821.6    26.1    118.6       0
4    off      256K   1825.7 399.8 73             0.0      0.0     0.0         0
4    on       256K   1776.8 410.8 1              821.4    26.3    120.6       0
4    off      1M     1632.7 447.6 289            0.0      0.0     0.0         0
4    on       1M     1611.2 453.2 1              823.5    26.7    269.4       0
```

Conclusion:

- Destination coalescing reduced QAT compression destination plus scratch
  output buffers from `37/73/289` to `1` for `128K/256K/1M`.
- Destination coalescing had zero allocation failures in the tested matrix.
- The copied compressed output was small because the TIFF source compresses
  well, but allocation and free time were material.
- Single-job results were mixed: 128K improved, while 256K and 1M were
  effectively flat to slightly slower. Four-job results improved modestly at all
  tested record sizes.
- Keep `zfs_qat_dc_coalesce_dst=0` by default. It is useful as an experimental
  knob and possible future throughput/recordsize policy input, but it is not a
  standalone latency fix.

## QAT Destination Coalescing Reuse Follow-Up

Run date: 2026-05-15.

The destination coalescing path now reuses contiguous destination output buffers
from the QAT DC buffer-slot pool. The reuse slot count was increased from `4`
to `32` per DC instance to match the observed Phase 4 compression in-flight
depth. If all reusable slots are busy, the code falls back to per-request
destination coalescing allocation; if that allocation fails, it falls back to the
normal fragmented destination plus scratch path.

New QAT DC kstats:

```text
dc_compress_dst_coalesce_reuse_hits
dc_compress_dst_coalesce_reuse_misses
```

Host source backup before installing the destination-coalescing reuse build:

```text
/root/zfs-2.4.99.pre-dst-coalesce-reuse.20260515T093414Z
/root/zfs-2.4.99.pre-dst-coalesce-reuse.latest -> /root/zfs-2.4.99.pre-dst-coalesce-reuse.20260515T093414Z
```

Build and install logs:

```text
/root/zfs-qat-dst-coalesce-reuse-dkms-build-20260515.log
/root/zfs-qat-dst-coalesce-reuse-dkms-install-20260515.log
/root/zfs-qat-dst-coalesce-reuse-initramfs-20260515.log
/root/zfs-qat-dst-coalesce-reuse-dkms-build-20260515-r2.log
/root/zfs-qat-dst-coalesce-reuse-dkms-install-20260515-r2.log
/root/zfs-qat-dst-coalesce-reuse-initramfs-20260515-r2.log
```

Loaded module after the final DKMS install, `update-initramfs -u -k
7.0.0-3-pve`, and reboot:

```text
srcversion: 51222238CFF46E75D24E091
```

The `/nvme_scratch` pool had been recreated empty. The TIFF test source was
restored from the lz4-backed copy:

```text
/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif
sha256 dec26817a6c7d6c6193c6db7e740e5d82bf19166a1ebab8f9af75cad80c01d6f
```

Source CSVs:

```text
/root/zfs-qat-phase4-dst-coalesce-reuse-smoke-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-reuse-smoke-r2-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-reuse-warmup-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-reuse-off-jobs1-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-reuse-on-jobs1-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-reuse-off-jobs4-20260515.csv
/root/zfs-qat-phase4-dst-coalesce-reuse-on-jobs4-20260515.csv
```

Summary:

```text
jobs coalesce record avg_ms MiB_s dst_total_bufs reuse_hit reuse_miss alloc_MB copy_MB dc_fails
1    off      128K   836.6  218.1 37             0         0          0.0      0.0     0
1    on       128K   797.9  228.8 1              1460      0          0.0      6.5     0
1    off      256K   698.3  261.3 73             0         0          0.0      0.0     0
1    on       256K   699.7  261.6 1              730       0          0.0      6.6     0
1    off      1M     622.1  293.4 289            0         0          0.0      0.0     0
1    on       1M     658.8  277.7 1              183       0          0.0      6.7     0
4    off      128K   1262.8 578.1 37             0         0          0.0      0.0     0
4    on       128K   1324.0 551.8 1              5646      194        27.2     26.1    0
4    off      256K   1249.0 584.6 73             0         0          0.0      0.0     0
4    on       256K   1255.2 581.8 1              2836      84         23.5     26.3    0
4    off      1M     1160.5 629.7 289            0         0          0.0      0.0     0
4    on       1M     1191.7 612.5 1              707       25         28.5     26.7    0
```

Conclusion:

- Increasing reuse slots to `32` per DC instance fixed the first smoke issue
  where four slots caused many per-request destination coalescing allocations.
- Warmed single-job runs had zero destination coalescing allocation bytes.
- Four-job runs still had some misses, but allocation volume was reduced to
  about `23-29 MiB`.
- The elapsed result was not a clear win: one-job 128K improved, one-job 256K
  was flat, one-job 1M regressed, and all four-job records regressed.
- Keep `zfs_qat_dc_coalesce_dst=0` by default. Destination buffer-list shaping
  should be parked unless later async or queue-depth work changes the tradeoff.

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
- Optional concurrent copy/verify jobs through the `JOBS` environment variable.
- Summary latency columns: average, p50, p95, p99, and max.
- CPU user/system/iowait/idle percentages.
- Compression ratio, used space, and logical used space.
- QAT compression/decompression kstat deltas and DC failure deltas.
- `zfs_qat_cpa_dc_level`, `zfs_qat_dc_max_buf_size`, `zfs_qat_dc_max_instances`, and loaded module `srcversion`.

Smoke test:

```text
ITERS=1 RECORDS=256K MODES=qat OUT=/root/zfs-qat-phase4-harness-smoke-20260513-r3.csv /root/qat-phase4-benchmark.sh
```

The initial smoke CSV had 32 columns for header, raw, and summary rows, moved
QAT compression counters for the 256 KiB TIFF workload, and completed without
leaving a `qat-phase4` temporary dataset behind. Later reuse counters expanded
the harness output to 34 columns. Later `JOBS` support expanded it to 35
columns.

## Non-Serializing Reuse Follow-Up

Run date: 2026-05-13.

The compression path now preallocates a small lock-free reuse pool per active DC
instance for QAT buffer-list metadata and `CpaBufferList`/`CpaFlatBuffer`
storage. Each request attempts to claim a slot with `test_and_set_bit()`.
If all slots are busy, the request falls back to the existing per-request
allocation path. This avoids the previously abandoned mutex-protected workspace
shape.

New QAT kstats:

```text
dc_buffer_reuse_hits
dc_buffer_reuse_misses
```

Host backup before refreshing `/usr/src/zfs-2.4.99/`:

```text
/root/zfs-2.4.99.pre-dc-reuse.20260513T044340Z
/root/zfs-2.4.99.pre-dc-reuse.latest -> /root/zfs-2.4.99.pre-dc-reuse.20260513T044340Z
```

Build and install logs:

```text
/root/zfs-qat-dc-reuse-dkms-build-20260513.log
/root/zfs-qat-dc-reuse-dkms-install-20260513.log
/root/zfs-qat-dc-reuse-initramfs-20260513.log
```

Loaded module after DKMS install, `update-initramfs -u -k 7.0.0-3-pve`, and reboot:

```text
filename: /lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko
version: 2.4.99-1
srcversion: 3A6C45EADEB1E3DF1D6F3C9
depends: spl,qat_api
```

Smoke command:

```text
ITERS=1 RECORDS="256K 1M" MODES=qat OUT=/root/zfs-qat-phase4-reuse-smoke-20260513.csv /root/qat-phase4-benchmark.sh
```

Smoke results:

```text
record comp_req decomp_req dc_fails reuse_hits reuse_misses sha_ok
256K   730      578        0        818        490           yes
1M     183      153        0        214        122           yes
```

The smoke CSV had 34 columns for header, raw, and summary rows. Temporary
datasets were destroyed and `zpool status -x` reported all pools healthy.
The misses are expected when all reuse slots are busy or the pool cannot satisfy
a request; they are fallback allocations, not QAT failures.

## Concurrent Benchmark Follow-Up

Run date: 2026-05-13.

The benchmark harness now supports concurrent copy/verify jobs:

```text
JOBS=4
```

Concurrent comparison command:

```text
ITERS=3 JOBS=4 RECORDS="128K 256K 1M" MODES="qat sw" OUT=/root/zfs-qat-phase4-concurrent-20260513.csv /root/qat-phase4-benchmark.sh
```

Summary:

```text
record qat_ms sw_ms  qat_bw sw_bw  qat_sys sw_sys dc_fails
128K   1350.7 1079.1 541.8  677.4  2.99    12.50  0
256K   1265.3 991.0  576.9  737.7  2.41    13.09  0
1M     1227.3 935.8  594.9  780.2  1.84    13.67  0
```

Result: QAT remained correct and used much less system CPU, but software gzip
remained faster under four concurrent copy/verify jobs. The 4-job QAT gap was
about `25-31%` slower by wall-clock latency and `20-24%` lower by aggregate
throughput.

## Compression Level Matrix Follow-Up

Run date: 2026-05-15.

This follow-up compared QAT compression levels 1 through 4 after destination
coalescing reuse was installed, with source and destination coalescing disabled.
The host boot configuration was backed up before the matrix and restored after
the run:

```text
/etc/modprobe.d/zfs-qat.conf.pre-level-matrix-20260515
/root/zfs-qat-level-matrix-restore-initramfs-20260515.log
```

Raw CSVs:

```text
/root/zfs-qat-phase4-level1-jobs1-20260515.csv
/root/zfs-qat-phase4-level1-jobs4-20260515.csv
/root/zfs-qat-phase4-level2-jobs1-20260515.csv
/root/zfs-qat-phase4-level2-jobs4-20260515.csv
/root/zfs-qat-phase4-level3-jobs1-20260515.csv
/root/zfs-qat-phase4-level3-jobs4-20260515.csv
/root/zfs-qat-phase4-level4-jobs1-20260515.csv
/root/zfs-qat-phase4-level4-jobs4-20260515.csv
```

Module settings for each run:

```text
zfs_qat_cpa_dc_level=1..4
zfs_qat_cpa_dc_hufftype=dynamic
zfs_qat_dc_max_buf_size=1048576
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
```

Summary:

```text
jobs record fastest_level fastest_ms fastest_MiB_s level4_ms level1_ratio level4_ratio
1    128K   3             682.8      267.5         729.8     16.88x       17.11x
1    256K   1             639.1      286.2         674.3     21.63x       21.89x
1    1M     3             597.8      305.3         682.4     25.15x       25.47x
4    128K   1             1208.1     604.3         1273.5    16.92x       17.16x
4    256K   2             1139.8     640.7         1210.5    21.70x       21.96x
4    1M     1             1069.6     682.7         1141.7    25.25x       25.56x
```

Observed QAT compression wait time followed the same general pattern: level 4
had the highest accumulated wait time in most rows. Single-job 1M averaged
`1930.8 ms` at level 1 and `2564.1 ms` at level 4; four-job 1M averaged
`15904.0 ms` at level 1 and `20180.0 ms` at level 4.

All level-matrix CSVs had the expected 90 columns and reported `dc_fails=0`.
The host was restored to:

```text
options zfs zfs_qat_compress_disable=0 zfs_qat_checksum_disable=0 zfs_qat_cpa_dc_level=4 zfs_qat_dc_max_buf_size=1048576
```

Result:

- No single QAT compression level was fastest for all tested cases.
- Level 4 gave the best ratio, but the ratio gain over level 1 was only about
  `1.2-1.4%` for the TIFF workload and came with worse elapsed time in the
  fastest-row comparison.
- Level 1 is the best current candidate for a performance-biased policy under
  concurrent work.
- Level 3 was fastest for single-job 128K and 1M in this run, but it did not
  hold under four jobs.
- Keep using the explicit global `zfs_qat_cpa_dc_level` parameter while the
  performance/ratio policy is still being proven.

## Best-Case Level 1 Fair Comparison

Run date: 2026-05-16.

This follow-up temporarily booted the host with level 1 and compared QAT against
software gzip in the same benchmark window. Software readback verification was
used so the result tested QAT compression without mixing in QAT decompression
policy.

Raw CSVs:

```text
/root/zfs-qat-phase4-level1-bestcase-jobs1-20260516.csv
/root/zfs-qat-phase4-level1-bestcase-jobs4-20260516.csv
```

Module settings:

```text
zfs_qat_cpa_dc_level=1
zfs_qat_cpa_dc_hufftype=dynamic
zfs_qat_dc_max_buf_size=1048576
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
```

Summary:

```text
jobs record qat_ms  sw_ms   qat_vs_sw qat_MiB_s sw_MiB_s qat_sys sw_sys qat_ratio sw_ratio
1    128K   758.9   766.6   -1.0%     240.7     238.2    1.87    4.13   16.88x    16.86x
1    256K   693.9   599.7   +15.7%    263.1     304.8    1.51    4.65   21.63x    21.53x
1    1M     624.0   540.4   +15.5%    292.5     337.8    1.41    4.92   25.15x    25.17x
4    128K   1208.2  1071.8  +12.7%    604.5     682.4    4.49    12.76  16.92x    16.90x
4    256K   1131.2  994.3   +13.8%    645.4     735.7    3.95    13.37  21.70x    21.60x
4    1M     1095.1  904.4   +21.1%    666.8     807.1    3.65    13.80  25.25x    25.26x
```

Negative `qat_vs_sw` means QAT was faster. All rows passed verification and QAT
reported `dc_fails=0`. The two CSVs had the expected 90 columns and 24 data rows
each.

The host was restored to the pre-test level 4 boot/runtime configuration after
the comparison:

```text
options zfs zfs_qat_compress_disable=0 zfs_qat_checksum_disable=0 zfs_qat_cpa_dc_level=4 zfs_qat_dc_max_buf_size=1048576
```

Result:

- Level 1 QAT reached parity only at single-job 128K, where it was `1.0%`
  faster than software.
- Software remained faster for single-job 256K and 1M by `15-16%`.
- Software remained faster under four jobs by `13-21%`.
- QAT continued to use substantially less system CPU.
- Compression-level policy alone is not enough to meet the throughput and
  latency goals. The next target should be an async/queueing design spike.

## Follow-Up

- Continue phase 4 with throughput and latency as first-class requirements. Future benchmark output should include throughput, p50/p95/p99/max latency, CPU cost, compression ratio, QAT kstats, and failure counters.
- Keep larger-record QAT support as opt-in. Initial 256 KiB and 1 MiB validation proves QAT offload can work on this host, but the best-case level 1 comparison still favors software at larger records.
- Evaluate optimization bias parameters only after measurements identify real policy choices. A throughput/latency bias such as `latency`, `balanced`, and `throughput` is useful if queueing, batching, thresholds, or reuse strategies create measured tradeoffs. A performance/ratio bias such as `performance`, `balanced`, and `compressionratio` is useful if compression effort or fallback policy creates measured tradeoffs.
- Keep explicit low-level parameters for benchmarking first. Bias parameters should later set coherent defaults across those low-level knobs; they should not be added as no-op labels before the policies are proven.
- Move the next implementation spike toward async/queueing rather than additional small allocation or buffer-shape tuning. See `phase-4-async-queue-spike.md` for the first-pass design.
- Park NUMA performance tuning until a true multi-socket QAT 1.x host is available.

## Async QAT Write-Compression Smoke

Run date: 2026-05-16.

The first callback-driven async QAT gzip write-compression pass was built,
installed, booted, and smoke-tested on `pve.drewnet.online`. The feature remains
disabled by default behind `zfs_qat_dc_async=0`.

Raw CSVs:

```text
/root/zfs-qat-phase4-async-off-smoke-r2-20260516.csv
/root/zfs-qat-phase4-async-on-smoke-r2-20260516.csv
```

Summary:

```text
mode async record jobs elapsed_ms MiB_s  ratio  dc_fails async_submits submit_fails completions fallbacks verify
qat  0     128K   1    908.370    200.89 17.11x 0        0             0            0           0         yes
qat  1     128K   1    718.856    253.85 17.03x 0        1460          529          931         529       yes
```

Result:

- Async-on was `20.9%` faster than async-off for this smoke row.
- The async path completed and resumed ZIOs from QAT callbacks.
- Verification passed and all pools remained healthy.
- Submit failures are still high and currently fall back to software gzip.
- This is a smoke result only; the full phase 4 matrix is still required.

## Async Retry/Backoff Tuning

Run date: 2026-05-16.

Submit-failure instrumentation confirmed async submit failures are
`CPA_STATUS_RETRY`. No resource or generic submit failures were seen in the
retry probes.

Candidate default for async mode:

```text
zfs_qat_dc_async_submit_retries=8
zfs_qat_dc_async_retry_us=100
```

Raw CSVs:

```text
/root/zfs-qat-phase4-async-retry0-smoke-20260516.csv
/root/zfs-qat-phase4-async-retry2-smoke-20260516.csv
/root/zfs-qat-phase4-async-retry8-smoke-20260516.csv
/root/zfs-qat-phase4-async-retry8-us10-smoke-20260516.csv
/root/zfs-qat-phase4-async-retry8-us100-smoke-20260516.csv
/root/zfs-qat-phase4-async-retry16-us50-smoke-20260516.csv
/root/zfs-qat-phase4-async-retry32-us50-smoke-20260516.csv
/root/zfs-qat-phase4-async-8r100us-jobs1-20260516.csv
/root/zfs-qat-phase4-async-8r100us-jobs4-20260516.csv
```

128K retry probe summary:

```text
retries retry_us elapsed_ms submit_fails retry_success final_retry_fails
0       50       864.967    411          0             411
2       50       709.949    509          149           509
8       50       703.007    426          345           426
8       10       782.450    465          253           465
8       100      699.131    360          414           360
16      50       775.053    326          493           326
32      50       878.327    167          686           167
```

One-iteration `8/100` comparison:

```text
jobs record async_ms sw_ms   async_vs_sw async_MiB_s sw_MiB_s async_fallbacks verify
1    128K   780.894  709.321 +10.1%      233.68      257.26   331             yes
1    256K   709.884  649.872 +9.2%       257.06      280.80   16              yes
1    1M     674.051  563.668 +19.6%      270.72      323.74   0               yes
4    128K   1147.142 1178.060 -2.6%      636.30      619.60   3145            yes
4    256K   1041.147 1079.694 -3.6%      701.08      676.05   1548            yes
4    1M     1236.152 960.369  +28.7%     590.48      760.05   35              yes
```

Result:

- `8/100` is the best observed retry/backoff setting in the 128K single-row probes.
- Async QAT now beats software in the one-iteration concurrent 128K and 256K rows.
- Software still wins single-job rows and the concurrent 1M row.
- The remaining fallback count is high for concurrent small records, so retry/backoff alone is not enough. The next target should control async submit pressure rather than simply increasing retry count.

## Async In-Flight Cap Tuning

Run date: 2026-05-16.

The next async pass added:

```text
zfs_qat_dc_async_max_inflight=96
```

This cap limits the number of active async QAT compression requests. When the
cap is reached, ZFS skips async QAT for that block and falls back to software
gzip without first building and submitting a QAT request. The default remains
`zfs_qat_dc_async=0`, so this only affects explicitly enabled async testing.

Raw CSVs:

```text
/root/zfs-qat-phase4-async-cap32-jobs4-20260516.csv
/root/zfs-qat-phase4-async-cap64-jobs4-20260516.csv
/root/zfs-qat-phase4-async-cap96-jobs4-20260516.csv
/root/zfs-qat-phase4-async-cap128-jobs4-20260516.csv
/root/zfs-qat-phase4-async-cap192-jobs4-20260516.csv
/root/zfs-qat-phase4-async-cap256-jobs4-20260516.csv
/root/zfs-qat-phase4-async-cap512-jobs4-20260516.csv
/root/zfs-qat-phase4-async-cap96-jobs1-compare-20260516.csv
/root/zfs-qat-phase4-async-cap96-jobs4-compare-20260516.csv
```

Jobs=4 cap sweep:

```text
cap record async_ms MiB_s  async_submits completions cap_skips submit_fails verify
32  128K   1033.719 706.12 5840          1335        4505      0            yes
32  256K   964.506  756.79 2920          687         2233      0            yes
64  128K   1063.828 686.13 5840          1535        4305      0            yes
64  256K   936.620  779.32 2920          759         2161      0            yes
96  128K   1056.668 690.78 5840          1402        4438      0            yes
96  256K   924.537  789.51 2920          687         2233      0            yes
192 128K   1072.564 680.54 5840          1697        4143      0            yes
192 256K   972.536  750.54 2920          777         2143      0            yes
256 128K   1040.878 701.26 5840          1831        4009      0            yes
256 256K   954.047  765.09 2920          825         2095      0            yes
512 128K   1096.017 665.98 5840          1935        3727      178          yes
512 256K   982.047  743.27 2920          1082        1680      158          yes
```

Cap `96` was the best balanced choice in this pass: it produced the fastest
256K jobs=4 result and kept 128K jobs=4 clearly ahead of software while avoiding
QAT submit failures.

One-iteration cap-96 comparison:

```text
jobs record async_ms sw_ms   async_vs_sw async_MiB_s sw_MiB_s verify
1    128K   753.630  692.052 +8.9%       242.14      263.68   yes
1    256K   682.103  601.603 +13.4%      267.53      303.33   yes
1    1M     599.406  573.096 +4.6%       304.44      318.41   yes
4    128K   1073.745 1172.390 -8.4%      679.80      622.60   yes
4    256K   927.778  956.636  -3.0%      786.75      763.01   yes
4    1M     876.489  892.719  -1.8%      832.79      817.65   yes
```

Result:

- Admission control is better than retry-only pressure handling.
- Cap `96` avoids QAT submit failures in the measured jobs=4 cap sweep.
- Async QAT with cap `96` beats software in this one-iteration jobs=4 matrix
  as an adaptive hybrid QAT/software policy.
- Software still wins jobs=1, so async should not become a blanket policy.
- Cap-skipped blocks use software gzip directly. Therefore, rows with nonzero
  `dc_compress_async_cap_skips_delta` are not pure-QAT measurements. For
  example, `1402` completions and `4438` cap skips means `24.0%` QAT and
  `76.0%` software, calculated as `completions / async_submits` and
  `cap_skips / async_submits`.
- The next target should be policy selection: use async only when concurrency
  and record size make it likely to beat software, then validate with repeated
  iterations.

## Cap-96 Small-Record Follow-Up

Run date: 2026-05-17.

Source CSVs:

```text
/root/zfs-qat-phase4-async-cap96-small-jobs1-20260517.csv
/root/zfs-qat-phase4-async-cap96-small-jobs4-20260517.csv
```

Test settings:

```text
zfs_qat_dc_async=1
zfs_qat_dc_async_submit_retries=8
zfs_qat_dc_async_retry_us=100
zfs_qat_dc_async_max_inflight=96
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
VERIFY_MODE=sw
ITERS=1
RECORDS="8K 16K 32K 64K"
MODES="qat sw"
```

Single-job results:

```text
record async_ms sw_ms    async_vs_sw qat_share cap_skips submit_fails verify
8K     1668.786 1541.165 +8.3%       100.0%    0         0            yes
16K    1299.723 1160.976 +12.0%      99.8%     26        0            yes
32K    1105.450 1169.066 -5.4%       57.3%     2495      0            yes
64K    976.121  837.671  +16.5%      41.1%     1719      0            yes
```

Four-job results:

```text
record async_ms sw_ms    async_vs_sw qat_share cap_skips submit_fails verify
8K     2422.484 2344.700 +3.3%       32.7%     62900     0            yes
16K    1791.121 1617.184 +10.8%      25.6%     34759     0            yes
32K    1790.974 1709.059 +4.8%       35.2%     15136     0            yes
64K    1339.324 1219.847 +9.8%       26.0%     8640      0            yes
```

Result:

- Smaller records did not make Cap-96 async QAT faster than software in this
  matrix. Software won every four-job row and three of four single-job rows.
- The only winning small-record row was single-job `32K`, but it was already a
  mixed row with `57.3%` QAT completions and `42.7%` cap-skipped software
  fallback.
- Single-job `8K` was a pure-QAT row and was `8.3%` slower than software. This
  is useful evidence that the current QAT 1.x path is not merely starved by
  large records; per-block QAT service/setup cost is also material at small
  records.
- Under four jobs, all small-record Cap-96 rows were mostly software fallback,
  with only `25.6-35.2%` of async submissions completing through QAT.
- Future benchmarks should report QAT share next to latency and throughput
  whenever async admission control is enabled. Pure-QAT behavior and adaptive
  hybrid policy behavior should be evaluated separately.

## DC-Only 6-Instance Cap-96 Follow-Up

Run date: 2026-05-17.

The host was switched from the prior `[KERNEL_QAT]` split of four crypto and
two compression instances to a DC-only split:

```text
NumberCyInstances = 0
NumberDcInstances = 6
```

ZFS QAT crypto/checksum acceleration was disabled for this host test:

```text
zfs_qat_checksum_disable=1
zfs_qat_encrypt_disable=1
```

The driver accepted the split after reboot. The benchmark harness recorded
`qat_kernel_cy_instances=0` and `qat_kernel_dc_instances=6` in the CSV rows.

Source CSVs:

```text
/root/zfs-qat-phase4-async-cap96-dc6-jobs1-20260517.csv
/root/zfs-qat-phase4-async-cap96-dc6-jobs4-20260517.csv
/root/zfs-qat-phase4-async-cap96-dc6-jobs4-repeat-20260517.csv
```

Comparable one-iteration Cap-96 results:

```text
jobs record async_ms sw_ms   async_vs_sw qat_share cap_skips submit_fails verify
1    128K   837.365  725.009 +15.5%      44.9%     804       0            yes
1    256K   641.044  641.013 +0.0%       50.8%     359       0            yes
1    1M     587.222  566.868 +3.6%       76.5%     43        0            yes
4    128K   1079.252 1089.128 -0.9%      30.4%     4064      0            yes
4    256K   1037.007 966.262  +7.3%      30.0%     2043      0            yes
4    1M     897.341  958.766  -6.4%      34.2%     482       0            yes
```

Three-iteration jobs=4 repeat:

```text
record async_avg_ms sw_avg_ms async_vs_sw qat_share cap_skips submit_fails verify
128K   1150.050     1152.593 -0.2%       29.9%     12285     0            yes
256K   981.900      974.846  +0.7%       28.7%     6248      0            yes
1M     882.406      953.739  -7.5%       35.5%     1416      0            yes
```

Result:

- It is possible to use all six QAT API kernel instances for DC and zero for
  crypto on this dh895xcc host.
- The six-DC split increased the QAT completion share relative to the earlier
  two-DC Cap-96 run, but the rows are still mostly software fallback under
  `zfs_qat_dc_async_max_inflight=96`.
- The six-DC split did not produce a broad Cap-96 win. The repeated jobs=4 run
  was effectively parity at `128K`, slightly slower at `256K`, and faster at
  `1M`.
- Submit failures remained zero, so the cap is still preventing QAT retry
  pressure.
- This result does not justify assuming that more DC instances alone will solve
  the latency gap. It may be useful for larger records, but the policy still
  needs record-size and concurrency gating.

### DC6 Small-Record Cap-96 Follow-Up

Source CSVs:

```text
/root/zfs-qat-phase4-async-cap96-dc6-small-jobs1-20260517.csv
/root/zfs-qat-phase4-async-cap96-dc6-small-jobs4-20260517.csv
/root/zfs-qat-phase4-async-cap96-dc6-small-jobs4-repeat-20260517.csv
```

Single-job one-iteration results:

```text
record async_ms sw_ms    async_vs_sw qat_share cap_skips submit_fails verify
8K     1526.686 1521.959 +0.3%       99.7%     70        0            yes
16K    1184.087 1174.081 +0.9%       99.9%     7         0            yes
32K    1055.309 1076.144 -1.9%       67.7%     1885      0            yes
64K    954.536  892.426  +7.0%       40.1%     1749      0            yes
```

Four-job one-iteration results:

```text
record async_ms sw_ms    async_vs_sw qat_share cap_skips submit_fails verify
8K     2365.443 2392.117 -1.1%       41.5%     54628     0            yes
16K    1723.020 1603.424 +7.5%       28.6%     33364     0            yes
32K    1823.673 1631.320 +11.8%      37.5%     14605     0            yes
64K    1391.715 1211.059 +14.9%      34.4%     7661      0            yes
```

Four-job three-iteration repeat:

```text
record async_avg_ms sw_avg_ms async_vs_sw qat_share cap_skips submit_fails verify
8K     2346.487     2339.104 +0.3%       58.8%     115566    0            yes
16K    1713.810     1601.833 +7.0%       31.4%     96085     0            yes
32K    1769.260     1717.969 +3.0%       36.3%     44648     0            yes
64K    1358.403     1228.190 +10.6%      30.5%     24355     0            yes
```

Result:

- DC6 small-record Cap-96 is not a performance win. The repeated jobs=4 matrix
  is effectively parity at `8K` and slower than software at `16K`, `32K`, and
  `64K`.
- Six DC instances made the single-job `8K` and `16K` rows nearly pure-QAT and
  nearly equal to software, which is a material improvement over the earlier
  two-DC small-record run. It still did not beat software.
- The repeated jobs=4 `8K` row completed more QAT work than software fallback
  (`58.8%` QAT), but elapsed time was still slightly slower than software.
- Submit failures remained zero. The cap is controlling queue pressure, but
  small-record QAT service/setup cost remains too high to beat software gzip.
- Combined with the larger-record DC6 repeat, the current policy evidence
  favors DC6 Cap-96 only for larger records such as `1M`, not for small records.

### DC6 Async Cap Sweep

Run date: 2026-05-17.

Source CSVs:

```text
/root/zfs-qat-phase4-async-dc6-cap96-sweep-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap192-sweep-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap384-sweep-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap768-sweep-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap0-sweep-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap192-target-repeat-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap384-target-repeat-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-cap768-target-repeat-jobs4-20260517.csv
```

Test settings:

```text
NumberCyInstances = 0
NumberDcInstances = 6
zfs_qat_dc_async=1
zfs_qat_dc_async_submit_retries=8
zfs_qat_dc_async_retry_us=100
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
zfs_qat_decompress_disable=1
VERIFY_MODE=sw
JOBS=4
```

One-iteration sweep:

```text
cap record async_ms sw_ms   async_vs_sw qat_share cap_skips submit_fails retries verify
96  8K     2402.2   2364.3  +1.6%       41.5%     54671     0            0       yes
96  16K    1715.5   1593.4  +7.7%       26.5%     34331     0            0       yes
96  32K    1917.6   1647.2  +16.4%      37.5%     14596     0            0       yes
96  64K    1352.3   1184.0  +14.2%      28.6%     8338      0            0       yes
96  128K   1119.1   1027.9  +8.9%       24.9%     4383      0            0       yes
96  256K   1032.7   992.4   +4.1%       25.2%     2185      0            0       yes
96  1M     925.1    905.6   +2.2%       35.0%     476       0            0       yes
192 32K    1697.5   1716.1  -1.1%       39.1%     14229     0            0       yes
192 128K   1054.1   1051.4  +0.3%       25.8%     4335      0            0       yes
192 256K   977.7    965.4   +1.3%       27.3%     2123      0            0       yes
384 256K   970.8    992.9   -2.2%       33.8%     1934      0            0       yes
768 128K   1132.7   1133.8  -0.1%       37.3%     3660      0            0       yes
768 1M     1208.1   974.2   +24.0%      100.0%    0         0            0       yes
0   8K     2564.2   2306.5  +11.2%      94.8%     0         4872         145431  yes
0   1M     1134.0   888.3   +27.7%      100.0%    0         0            0       yes
```

The full one-iteration sweep is in the CSV files above. The table shows every
cap-96 row, the only non-cap-96 rows that were close or faster than software,
and the uncapped boundary rows.

Targeted three-iteration repeats:

```text
cap record async_avg_ms sw_avg_ms async_vs_sw qat_share cap_skips submit_fails retries verify
192 32K    1906.1       1686.5    +13.0%      41.2%     41184     0            0       yes
192 128K   1072.5       1067.2    +0.5%       26.8%     12822     0            0       yes
192 256K   955.6        986.0     -3.1%       27.1%     6384      0            0       yes
192 1M     977.4        944.3     +3.5%       45.7%     1193      0            0       yes
384 32K    1900.4       1646.2    +15.4%      40.4%     41793     0            0       yes
384 128K   1084.4       1059.1    +2.4%       28.7%     12488     0            0       yes
384 256K   965.7        948.9     +1.8%       33.4%     5838      0            0       yes
384 1M     1092.0       954.1     +14.4%      73.4%     584       0            0       yes
768 32K    1893.1       1669.0    +13.4%      37.2%     43988     0            0       yes
768 128K   1060.9       1096.3    -3.2%       37.5%     10957     0            0       yes
768 256K   988.4        982.3     +0.6%       47.6%     4586      0            0       yes
768 1M     1178.6       933.7     +26.2%      100.0%    0         0            0       yes
```

Result:

- Raising the cap increases QAT participation in some rows, but it does not
  produce a general performance win.
- Cap `192` produced the best repeated `256K` result in this pass, `3.1%`
  faster than software, but it was slower at `32K`, `128K`, and `1M`.
- Cap `768` produced the best repeated `128K` result in this pass, `3.2%`
  faster than software, but it was much slower at `32K` and `1M`.
- Cap `96` remains the best repeated `1M` result observed so far in the DC6
  tests: `7.5%` faster than software in the prior repeat.
- Uncapped mode (`zfs_qat_dc_async_max_inflight=0`) is not viable for this
  workload. It removes cap skips but reintroduces thousands of final submit
  failures and heavy retry traffic for smaller records, and it is slower than
  software even at `1M`.
- The useful cap appears to be record-size dependent. A single global cap is
  unlikely to be optimal across `8K` through `1M`.

### DC6 Recordsize Cap Policy

Run date: 2026-05-17.

Source CSVs:

```text
/root/zfs-qat-phase4-async-dc6-policy-fixed-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-policy-recordsize-jobs4-20260517.csv

Repo copies:
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-dc6-policy-fixed-jobs4-20260517.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-dc6-policy-recordsize-jobs4-20260517.csv
```

Test settings:

```text
NumberCyInstances = 0
NumberDcInstances = 6
zfs_qat_dc_async=1
zfs_qat_dc_async_submit_retries=8
zfs_qat_dc_async_retry_us=100
zfs_qat_dc_async_max_inflight=96
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
zfs_qat_decompress_disable=1
VERIFY_MODE=sw
JOBS=4
ITERS=3
```

The new policy knob is:

```text
zfs_qat_dc_async_cap_policy=fixed|recordsize
```

`fixed` preserves the existing behavior and uses
`zfs_qat_dc_async_max_inflight` for every eligible record. `recordsize` is a
QAT 1.x policy that includes the active DC instance count. On the DC6 host it
skips async QAT for records below `128K`, uses a `768` cap for `128K`, a `192`
cap for `256K`, and a `96` cap for `1M` and larger records. Untested instance
counts and intermediate record sizes fall back to the fixed cap.

Three-iteration jobs=4 comparison:

```text
policy     record qat_avg_ms sw_avg_ms qat_vs_sw qat_share cap_skips ratio_qat ratio_sw
fixed      8K     2432.329   2307.079  +5.4%     49.7%     141015    4.43x     4.44x
fixed      16K    1685.861   1609.556  +4.7%     27.9%     101010    7.83x     7.83x
fixed      32K    1857.981   1699.187  +9.3%     36.7%     44348     6.91x     6.89x
fixed      64K    1377.161   1268.829  +8.5%     28.9%     24912     11.60x    11.56x
fixed      128K   1090.465   1087.621  +0.3%     24.7%     13192     16.99x    16.90x
fixed      256K   967.729    982.396   -1.5%     24.9%     6583      21.71x    21.60x
fixed      1M     858.618    940.062   -8.7%     33.5%     1460      25.36x    25.26x
recordsize 8K     2329.554   2297.649  +1.4%     0.0%      280296    4.44x     4.44x
recordsize 16K    1635.570   1638.606  -0.2%     0.0%      140148    7.83x     7.83x
recordsize 32K    1633.441   1693.153  -3.5%     0.0%      70080     6.89x     6.89x
recordsize 64K    1243.085   1306.951  -4.9%     0.0%      35040     11.56x    11.56x
recordsize 128K   1068.960   1084.435  -1.4%     35.9%     11223     17.00x    16.90x
recordsize 256K   950.537    973.445   -2.4%     27.3%     6372      21.74x    21.60x
recordsize 1M     901.087    977.362   -7.8%     32.2%     1489      25.34x    25.26x
```

Relative to the fixed policy:

```text
record fixed_ms recordsize_ms recordsize_vs_fixed
8K     2432.329 2329.554      4.2% faster
16K    1685.861 1635.570      3.0% faster
32K    1857.981 1633.441      12.1% faster
64K    1377.161 1243.085      9.7% faster
128K   1090.465 1068.960      2.0% faster
256K   967.729  950.537       1.8% faster
1M     858.618  901.087       4.9% slower
```

Simple latency chart, lower is better:

```text
8K   fixed QAT      | ######################## 2432 ms
8K   recordsize QAT | #######################  2330 ms
8K   software       | #######################  2298 ms
32K  fixed QAT      | ##################       1858 ms
32K  recordsize QAT | ################         1633 ms
32K  software       | #################        1693 ms
128K fixed QAT      | ###########              1090 ms
128K recordsize QAT | ###########              1069 ms
128K software       | ###########              1084 ms
256K fixed QAT      | ##########               968 ms
256K recordsize QAT | #########                951 ms
256K software       | ##########               973 ms
1M   fixed QAT      | #########                859 ms
1M   recordsize QAT | #########                901 ms
1M   software       | ##########               977 ms
```

Result:

- The recordsize policy materially improves the small-record rows by avoiding
  QAT below `128K`. Those rows are now software-compressed from the QAT mode,
  which is why `qat_share` is `0.0%`.
- The policy improves the repeated `128K` and `256K` rows in this same-window
  run, and both beat software by a small margin.
- The `1M` row remains faster than software, but it was slower than fixed cap
  `96` in this pass. Since both policies should use cap `96` at `1M`, this is
  likely run-to-run noise or workload ordering rather than evidence that the
  policy should alter `1M`.
- `dc_compress_async_cap_skips_delta` now includes both queue-cap skips and
  policy skips. For `recordsize` rows below `128K`, the skips are deliberate
  policy skips to software gzip.
- `dc_compress_async_inflight_max` is cumulative for the loaded module, not a
  per-row cap trace. It should not be used by itself to infer the cap selected
  for each row after a higher-cap row has executed.

### DC6 1M Policy Repeat

Run date: 2026-05-17.

Source CSVs:

```text
/root/zfs-qat-phase4-async-dc6-policy-fixed-1m-repeat-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-policy-recordsize-1m-repeat-jobs4-20260517.csv

Repo copies:
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-dc6-policy-fixed-1m-repeat-jobs4-20260517.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-async-dc6-policy-recordsize-1m-repeat-jobs4-20260517.csv
```

Focused repeat settings:

```text
NumberCyInstances = 0
NumberDcInstances = 6
zfs_qat_dc_async=1
zfs_qat_dc_async_submit_retries=8
zfs_qat_dc_async_retry_us=100
zfs_qat_dc_async_max_inflight=96
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
zfs_qat_decompress_disable=1
VERIFY_MODE=sw
JOBS=4
ITERS=6
RECORDS=1M
```

Results:

```text
policy     mode avg_ms  MiB_s qat_share cap_skips ratio
fixed      qat  933.789 781.7 33.3%     2928      25.39x
fixed      sw   927.323 787.1 n/a       n/a       25.26x
recordsize qat 907.026 804.7 33.4%     2926      25.35x
recordsize sw  921.995 791.7 n/a       n/a       25.26x
```

Result: fixed and recordsize both select cap `96` for `1M`. The earlier
same-window difference was not a policy signal. Keep the DC6 `1M+` cap at `96`
in the recordsize policy.

### Async-Compatible Coalescing

Run date: 2026-05-17.

Change:

- `qat_dc_compress_async_enabled()` no longer rejects requests when
  `zfs_qat_dc_coalesce_src` or `zfs_qat_dc_coalesce_dst` is enabled.
- The async submit path now uses the existing source coalescing helper,
  destination coalescing helper, destination buffer-slot reuse, and copy-back
  accounting.
- If source or destination coalescing allocation fails, the async path falls
  back to the normal fragmented-buffer path.

Host source backup before installing the async-compatible coalescing build:

```text
/root/zfs-2.4.99.pre-async-coalesce.20260517T072844Z
/root/zfs-2.4.99.pre-async-coalesce.latest -> /root/zfs-2.4.99.pre-async-coalesce.20260517T072844Z
```

Build and install logs:

```text
/root/zfs-qat-async-coalesce-dkms-build-20260517.log
/root/zfs-qat-async-coalesce-dkms-install-20260517.log
/root/zfs-qat-async-coalesce-initramfs-20260517.log
```

Loaded module after DKMS install, `update-initramfs -u -k 7.0.0-3-pve`, and
reboot:

```text
srcversion: 18635F01D8EFD4EBD6C7675
```

Smoke CSVs:

```text
/root/zfs-qat-phase4-async-src-coalesce-smoke-20260517.csv
/root/zfs-qat-phase4-async-dst-coalesce-smoke-20260517.csv
```

Smoke result:

- Async plus source coalescing passed SHA verification and recorded
  `dc_compress_src_buffers_max=1`.
- Async plus destination coalescing passed SHA verification and recorded
  `dc_compress_dst_total_buffers_max=1`.

Targeted jobs=4 CSVs:

```text
/root/zfs-qat-phase4-async-coalesce-off-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-src-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-dst-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-both-jobs4-20260517.csv

Repo copies are under:
.codex/skills/openzfs-qat/references/benchmarks/
```

Three-iteration jobs=4 comparison:

```text
case record qat_ms   sw_ms    qat_vs_sw qat_share src_bufs dst_total_bufs
off  128K   1112.168 1091.258 +1.9%     40.1%     32.0     37.0
off  256K   1013.292 1008.034 +0.5%     30.8%     64.0     73.0
off  1M     986.959  914.903  +7.9%     33.3%     256.0    289.0
src  128K   1096.154 1058.649 +3.5%     36.6%     1.0      37.0
src  256K   975.573  964.011  +1.2%     27.1%     1.0      73.0
src  1M     876.813  928.709  -5.6%     32.7%     1.0      289.0
dst  128K   1161.276 1100.953 +5.5%     39.7%     32.0     1.0
dst  256K   979.081  1006.882 -2.8%     28.5%     64.0     1.0
dst  1M     933.987  1038.753 -10.1%    35.5%     256.0    1.0
both 128K   1201.735 1040.997 +15.4%    43.5%     1.0      1.0
both 256K   960.724  999.667  -3.9%     26.9%     1.0      1.0
both 1M     932.046  908.515  +2.6%     35.3%     1.0      1.0
```

Result:

- Async-compatible source and destination coalescing works functionally and
  passes SHA verification.
- Coalescing is not a universal performance win. It should remain disabled for
  automatic `128K` policy.
- `256K` improved with coalescing in this pass, with both source and
  destination coalescing producing the fastest QAT row.
- `1M` was best with source-only coalescing among QAT rows in this pass.
- Software baselines varied across the four coalescing runs, so these results
  should drive a follow-up repeat before making coalescing part of a default
  policy profile.

### Async Coalescing Focused Repeat

Run date: 2026-05-17.

Source CSVs:

```text
/root/zfs-qat-phase4-async-coalesce-off-focus-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-src-focus-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-dst-focus-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-both-focus-jobs4-20260517.csv

Repo copies are under:
.codex/skills/openzfs-qat/references/benchmarks/
```

Focused repeat settings:

```text
NumberCyInstances = 0
NumberDcInstances = 6
zfs_qat_dc_async=1
zfs_qat_dc_async_submit_retries=8
zfs_qat_dc_async_retry_us=100
zfs_qat_dc_async_max_inflight=96
zfs_qat_dc_async_cap_policy=recordsize
zfs_qat_decompress_disable=1
VERIFY_MODE=sw
JOBS=4
ITERS=6
RECORDS="256K 1M"
```

Results:

```text
case record qat_ms   sw_ms    qat_vs_sw qat_share src_bufs dst_total_bufs
off  256K   954.435  1064.672 -10.4%    27.0%     64.0     73.0
off  1M     924.087  1019.278 -9.3%     33.5%     256.0    289.0
src  256K   1000.454 1003.532 -0.3%     27.5%     1.0      73.0
src  1M     965.245  1083.916 -10.9%    33.7%     1.0      289.0
dst  256K   971.320  982.247  -1.1%     27.1%     64.0     1.0
dst  1M     960.360  932.231  +3.0%     33.5%     256.0    1.0
both 256K   986.401  987.878  -0.1%     27.7%     1.0      1.0
both 1M     911.303  909.951  +0.1%     35.4%     1.0      1.0
```

Result:

- The earlier coalescing signal did not hold in the focused repeat.
- Coalescing still works functionally, but it should remain an explicit manual
  knob rather than an automatic record-size policy behavior.
- The strongest `256K` row was coalescing off.
- The strongest `1M` QAT row was source+destination coalescing, but it was
  effectively equal to software and only modestly ahead of the off row in a
  noisy benchmark window.

### Benchmark Methodology Scorecard Run

Run date: 2026-05-18.

The benchmark harness now appends derived metrics for QAT byte share,
completion/fallback/cap-skip share, CPU seconds per GiB, and QAT service/wait
nanoseconds per MiB. This run validates the new interpretation rule: elapsed
wins with low QAT share are hybrid-policy wins, not QAT engine improvements.

Source CSV:

```text
/root/zfs-qat-phase4-methodology-current-policy-20260518.csv

Repo copy:
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-phase4-methodology-current-policy-20260518.csv
```

Test settings:

```text
NumberCyInstances = 0
NumberDcInstances = 6
zfs_qat_cpa_dc_level=4
zfs_qat_cpa_dc_hufftype=dynamic
zfs_qat_dc_async=1
zfs_qat_dc_async_submit_retries=8
zfs_qat_dc_async_retry_us=100
zfs_qat_dc_async_max_inflight=96
zfs_qat_dc_async_cap_policy=recordsize
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
zfs_qat_decompress_disable=1
VERIFY_MODE=sw
JOBS=4
ITERS=3
RECORDS="128K 256K 1M"
```

Results:

```text
record qat_ms   sw_ms    qat_vs_sw qat_cpu_s/GiB sw_cpu_s/GiB qat_byte_share fallback outcome
128K   1065.855 1050.781 +1.4%     10.001        12.192       37.1%          62.9%    cpu-offload-win
256K   953.253  987.853  -3.5%     9.585         11.772       27.0%          73.0%    hybrid-policy-win
1M     846.853  916.498  -7.6%     8.613         11.221       32.0%          68.1%    hybrid-policy-win
```

Result:

- The `256K` and `1M` QAT-labelled rows beat software and used materially less
  CPU, but they completed only `27.0-32.0%` of input bytes through QAT.
- These are valid hybrid-policy wins and CPU-offload wins. They are not proof
  that the QAT engine path itself improved.
- The `128K` row was `1.4%` slower than software but used `18.0%` fewer active
  CPU seconds per GiB, so it is best described as a CPU-offload win rather than
  a latency win.
- Compression ratio remained slightly better with QAT in all three rows:
  `17.01x` vs `16.90x`, `21.74x` vs `21.60x`, and `25.35x` vs `25.26x`.
- Future policy decisions should continue using QAT byte share and CPU seconds
  per GiB alongside elapsed time.

### Single-Card Scale Baseline

Run date: 2026-05-18.

This is the pre-install baseline for comparing one DH895XCC card against a
future two-card configuration.

Source CSVs:

```text
/root/zfs-qat-scale-single-card-jobs4-20260518.csv
/root/zfs-qat-scale-single-card-jobs8-20260518.csv

Repo copies:
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-scale-single-card-jobs4-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-scale-single-card-jobs8-20260518.csv
```

Scale state recorded in the CSV:

```text
qat_pci_dh895xcc_count=1
qat_conf_file_count=1
qat_kernel_cy_instances_total=0
qat_kernel_dc_instances_total=6
```

Test settings:

```text
zfs_qat_cpa_dc_level=4
zfs_qat_cpa_dc_hufftype=dynamic
zfs_qat_dc_async=1
zfs_qat_dc_async_submit_retries=8
zfs_qat_dc_async_retry_us=100
zfs_qat_dc_async_max_inflight=96
zfs_qat_dc_async_cap_policy=recordsize
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
zfs_qat_decompress_disable=1
VERIFY_MODE=sw
ITERS=3
RECORDS="128K 256K 1M"
MODES="qat sw"
```

Results:

```text
jobs record qat_ms   sw_ms    qat_vs_sw qat_cpu_s/GiB sw_cpu_s/GiB qat_byte fallback outcome
4    128K   1093.410 1061.340 +3.0%     10.104        12.319       36.7%    63.3%    cpu-offload-win
4    256K   985.632  969.663  +1.6%     9.558         11.568       26.7%    73.3%    cpu-offload-win
4    1M     868.412  927.999  -6.4%     8.535         11.250       32.4%    67.7%    hybrid-policy-win
8    128K   1561.836 1572.868 -0.7%     10.098        14.492       38.3%    61.7%    cpu-offload-win
8    256K   1459.295 1365.003 +6.9%     10.069        13.070       29.6%    70.5%    cpu-offload-win
8    1M     1631.944 1710.485 -4.6%     11.629        18.970       36.3%    64.0%    hybrid-policy-win
```

Result:

- Single-card QAT byte share remained low: `26.7-38.3%`.
- `1M` is the strongest elapsed-time hybrid-policy win at both `JOBS=4` and
  `JOBS=8`.
- `128K` and `256K` mostly remain CPU-offload wins, not latency wins.
- The dual-card test should use the same matrix and compare whether QAT byte
  share rises without increasing QAT service nanoseconds per MiB or CPU seconds
  per GiB.

### Dual-Card Scale Test

Run date: 2026-05-18.

After installing a second DH895XCC card, `/etc/dh895xcc_dev1.conf` was created
by copying the DC-only `/etc/dh895xcc_dev0.conf`. The host was rebooted so ZFS
QAT DC initialization could see both cards. Both devices were then up:

```text
qat_dev0: 0000:45:00.0, state up
qat_dev1: 0000:64:00.0, state up
```

Source CSVs:

```text
/root/zfs-qat-scale-dual-card-jobs4-20260518.csv
/root/zfs-qat-scale-dual-card-jobs8-20260518.csv

Repo copies:
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-scale-dual-card-jobs4-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-scale-dual-card-jobs8-20260518.csv
```

Scale state recorded in the CSV:

```text
qat_pci_dh895xcc_count=2
qat_conf_file_count=2
qat_kernel_cy_instances_total=0
qat_kernel_dc_instances_total=12
```

Dual-card results:

```text
jobs record qat_ms   sw_ms    qat_vs_sw qat_cpu_s/GiB sw_cpu_s/GiB qat_byte fallback outcome
4    128K   1112.824 1052.599 +5.7%     9.416         12.218       56.7%    43.3%    cpu-offload-win
4    256K   957.795  1018.140 -5.9%     8.400         11.936       44.7%    55.3%    hybrid-policy-win
4    1M     887.080  890.151  -0.3%     7.484         11.467       47.1%    53.1%    cpu-offload-win
8    128K   1571.288 1483.652 +5.9%     8.691         13.248       67.2%    32.8%    cpu-offload-win
8    256K   1383.344 1444.475 -4.2%     8.274         13.210       49.0%    51.0%    hybrid-policy-win
8    1M     1346.845 1455.752 -7.5%     8.399         12.132       48.2%    52.1%    hybrid-policy-win
```

Dual-card change versus single-card QAT rows:

```text
jobs record elapsed_delta qat_byte_delta cpu_delta service_delta wait_delta
4    128K   +1.8%         +20.0pp        -6.8%     -48.1%        -48.1%
4    256K   -2.8%         +17.9pp        -12.1%    -49.8%        -49.9%
4    1M     +2.1%         +14.7pp        -12.3%    -47.7%        -48.2%
8    128K   +0.6%         +28.9pp        -13.9%    -48.3%        -48.3%
8    256K   -5.2%         +19.4pp        -17.8%    -48.7%        -48.8%
8    1M     -17.5%        +12.0pp        -27.8%    -56.5%        -56.6%
```

Result:

- Scale works. The second card materially increased QAT byte share and reduced
  QAT service/wait nanoseconds per QAT-completed MiB by roughly half.
- More QAT hardware did not make `128K` a latency win. This reinforces that
  small-record per-request overhead remains material.
- `256K` benefits from scale, especially at `JOBS=8`.
- `1M` at `JOBS=8` is the strongest scale result: `17.5%` faster than the
  single-card QAT row, `7.5%` faster than same-window software, and `27.8%`
  lower CPU seconds per GiB than the single-card QAT row.
- The dual-card rows are still hybrid-policy rows because fallback remains
  `32.8-55.3%`. They are scale wins, not pure-QAT wins.

### DC Instance Cap-Scaling Test

Run date: 2026-05-18.

The previous dual-card run still used the conservative DC6 cap ceilings. A
follow-up build changed the record-size policy to scale linearly by active ZFS
DC instances:

```text
128K: 128 per DC instance
256K: 32 per DC instance
1M+: 16 per DC instance
```

On the two-card host this raised the effective caps from `768/192/96` to
`1536/384/192` for `128K/256K/1M+`.

Source CSVs:

```text
/root/zfs-qat-scale-dual-card-perinst-policy-jobs4-20260518.csv
/root/zfs-qat-scale-dual-card-perinst-policy-jobs8-20260518.csv

Repo copies:
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-scale-dual-card-perinst-policy-jobs4-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-scale-dual-card-perinst-policy-jobs8-20260518.csv
```

Scale state recorded in the CSV:

```text
qat_pci_dh895xcc_count=2
qat_conf_file_count=2
qat_kernel_cy_instances_total=0
qat_kernel_dc_instances_total=12
zfs_qat_dc_instances=12
```

Linear DC12 cap results:

```text
jobs record qat_ms   sw_ms    qat_vs_sw qat_cpu_s/GiB sw_cpu_s/GiB qat_byte fallback outcome
4    128K   1160.590 1102.165 +5.3%     9.532         11.502       91.9%    8.1%     regression
4    256K   1053.523 1038.453 +1.5%     9.128         11.216       65.8%    34.3%    regression
4    1M     958.740  969.450  -1.1%     6.723         10.740       75.4%    24.9%    cpu-offload-win
8    128K   1622.120 1522.491 +6.5%     9.484         13.208       82.1%    18.0%    regression
8    256K   1452.347 1503.671 -3.4%     9.063         13.946       62.9%    37.1%    hybrid-policy-win
8    1M     1360.608 1561.182 -12.8%    6.715         16.635       73.1%    27.5%    hybrid-policy-win
```

Linear DC12 cap change versus the previous dual-card QAT rows:

```text
jobs record elapsed_delta qat_byte_delta fallback_delta
4    128K   +4.3%         +35.3pp        -35.2pp
4    256K   +10.0%        +21.1pp        -21.1pp
4    1M     +8.1%         +28.3pp        -28.2pp
8    128K   +3.2%         +14.9pp        -14.9pp
8    256K   +5.0%         +13.9pp        -13.9pp
8    1M     +1.0%         +24.9pp        -24.7pp
```

Result:

- Linear scaling increased QAT byte share in every tested row.
- Linear scaling also made elapsed time worse in every row compared with the
  prior dual-card run.
- The default balanced policy should use active ZFS DC instance count for
  generality, but it should cap at the measured DC6 ceiling until a profile
  sweep proves higher caps are useful for a specific throughput or CPU-offload
  bias.
- The repo code was adjusted after this test to keep active-DC observability and
  DC-count cap calculation while limiting the balanced record-size policy to the
  DC6 measured ceiling.

### Cap Profile Midpoint Sweep

Run date: 2026-05-18.

This sweep tested caps between the conservative balanced DC6 ceiling and the
linear DC12 endpoint. It used fixed caps one record size at a time so each row
could be interpreted as a candidate for a future throughput/offload profile,
not as a default balanced-policy change.

Source CSVs:

```text
/root/zfs-qat-profile-cap-128k-cap1024-jobs8-20260518.csv
/root/zfs-qat-profile-cap-128k-cap1280-jobs8-20260518.csv
/root/zfs-qat-profile-cap-256k-cap256-jobs8-20260518.csv
/root/zfs-qat-profile-cap-256k-cap320-jobs8-20260518.csv
/root/zfs-qat-profile-cap-1m-cap128-jobs8-20260518.csv
/root/zfs-qat-profile-cap-1m-cap160-jobs8-20260518.csv

Repo copies:
.codex/skills/openzfs-qat/references/benchmarks/
```

Test settings:

```text
qat_pci_dh895xcc_count=2
qat_kernel_dc_instances_total=12
zfs_qat_dc_instances=12
zfs_qat_dc_async=1
zfs_qat_dc_async_cap_policy=fixed
zfs_qat_dc_async_submit_retries=8
zfs_qat_dc_async_retry_us=100
zfs_qat_decompress_disable=1
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
VERIFY_MODE=sw
JOBS=8
ITERS=3
```

Results:

```text
record cap  kind      qat_ms   sw_ms    qat_vs_sw qat_byte fallback sysCPU/GiB service_ns/MiB
128K   768  balanced  1571.288 1483.652 +5.9%     67.2%    32.8%    8.63       172067371
128K   1024 midpoint  1702.346 1749.028 -2.7%     76.9%    23.1%    9.75       217778741
128K   1280 midpoint  1581.038 1558.804 +1.4%     72.6%    27.4%    8.28       251317688
128K   1536 linear    1622.120 1522.491 +6.5%     82.1%    18.0%    9.42       281922002
256K   192  balanced  1383.344 1444.475 -4.2%     49.0%    51.0%    8.21       49353659
256K   256  midpoint  1481.648 1453.743 +1.9%     62.3%    37.8%    9.36       71320857
256K   320  midpoint  1475.804 1402.650 +5.2%     60.7%    39.4%    8.06       83981483
256K   384  linear    1452.347 1503.671 -3.4%     62.9%    37.1%    9.00       102687159
1M     96   balanced  1346.845 1455.752 -7.5%     48.2%    52.1%    8.33       25114634
1M     128  midpoint  1390.585 1452.624 -4.3%     57.2%    43.3%    7.54       30462370
1M     160  midpoint  1317.327 1400.008 -5.9%     63.3%    37.2%    7.18       34002147
1M     192  linear    1360.608 1561.182 -12.8%    73.1%    27.5%    6.67       39375024
```

Result:

- `128K`: higher caps increased QAT byte share but did not produce a stable
  throughput/latency result. Service cost rose with cap. Keep `128K` at the
  balanced cap for now.
- `256K`: both midpoint caps were worse than the balanced cap. Keep `256K` at
  the balanced cap.
- `1M`: cap `160` is the best new profile candidate. It produced the lowest
  elapsed time in this sweep, raised QAT byte share to `63.3%`, and lowered
  system CPU seconds per GiB versus the balanced cap.
- Cap `192` remains a CPU-offload candidate for `1M`, but it trades away some
  elapsed time versus cap `160`. It should not be the default throughput choice
  without a repeat.
- This does not change the balanced policy. It identifies `1M` cap `160` as the
  only cap-profile candidate worth repeating before adding a bias-profile knob.

### Cap Profile 1M Repeat

Run date: 2026-05-18.

The follow-up repeated `1M` caps `96`, `160`, and `192` with six iterations at
`JOBS=8`.

Source CSVs:

```text
/root/zfs-qat-profile-cap-1m-cap96-repeat-jobs8-20260518.csv
/root/zfs-qat-profile-cap-1m-cap160-repeat-jobs8-20260518.csv
/root/zfs-qat-profile-cap-1m-cap192-repeat-jobs8-20260518.csv

Repo copies:
.codex/skills/openzfs-qat/references/benchmarks/
```

Results:

```text
cap qat_ms   sw_ms    qat_vs_sw qat_byte fallback sysCPU/GiB service_ns/MiB outcome
96  1493.861 1483.583 +0.7%     57.6%    42.9%    8.76       25887884       regression
160 1399.584 1440.617 -2.8%     67.3%    33.2%    7.03       36893909       hybrid-policy-win
192 1284.636 1468.952 -12.5%    64.4%    36.1%    6.73       41482042       hybrid-policy-win
```

Result:

- Cap `160` did not repeat as the best candidate.
- Cap `192` was the fastest repeated `1M` result in this window and also had
  the lowest system CPU seconds per GiB.
- Higher service nanoseconds per QAT-completed MiB show this is not a pure QAT
  engine improvement. It is a hybrid-policy win for `1M` under `JOBS=8`.
- The code now has an explicit `zfs_qat_dc_async_cap_policy=throughput` mode.
  It preserves the balanced record-size policy for `128K`, `256K`, and untested
  `512K`, and only allows the higher linear active-DC cap for records of `1M`
  and larger.

### Profile Parameter Shell

Run date: 2026-05-18.

The first profile implementation slice added validated host-level profile
selectors and made async cap policy profile-managed by default:

```text
zfs_qat_dc_profile=balanced|latency|throughput|offload
zfs_qat_dc_profile_recordsize=131072|262144|524288|1048576
zfs_qat_dc_ratio_profile=balanced|performance|ratio
zfs_qat_dc_async_cap_policy=profile|fixed|recordsize|throughput
```

Default state:

```text
zfs_qat_dc_async=0
zfs_qat_dc_async_cap_policy=profile
zfs_qat_dc_profile=balanced
zfs_qat_dc_profile_recordsize=131072
zfs_qat_dc_ratio_profile=balanced
```

Effective behavior:

- Concrete `zfs_qat_dc_async_cap_policy` values still act as manual overrides.
- `zfs_qat_dc_async_cap_policy=profile` computes the cap policy from
  `zfs_qat_dc_profile` and `zfs_qat_dc_profile_recordsize`.
- `throughput` or `offload` with target record size `1048576` uses the measured
  higher `1M+` throughput cap behavior.
- All other initial profile combinations use balanced `recordsize` cap
  behavior.
- Async QAT remains disabled by default; this change does not make async writes
  active unless `zfs_qat_dc_async=1`.

Validation:

```text
srcversion: 16975019E0003C1241D4DF7
/root/zfs-qat-profile-params-smoke-20260518.csv
```

The host accepted valid profile, ratio-profile, target-recordsize, and
cap-policy values, rejected invalid values, and the benchmark harness smoke CSV
recorded the new profile columns with `127` aligned columns.

### Profile Max Buffer

Run date: 2026-05-18.

The second profile implementation slice made the QAT DC maximum buffer size
profile-managed by default:

```text
zfs_qat_dc_max_buf_size=profile|131072|262144|524288|1048576
```

Effective behavior:

- `profile` uses `zfs_qat_dc_profile_recordsize` as the effective maximum QAT
  DC input size.
- Concrete values remain manual overrides for this tunable only.
- Records larger than the effective value continue to fall back to software.
- The effective value must be fixed before QAT DC sessions are initialized;
  changes that would alter initialized session sizing are rejected with `EBUSY`.

Validation:

```text
srcversion: 3F7B9C471829F5421207D9D
/root/zfs-qat-profile-maxbuf-smoke-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-maxbuf-smoke-20260518.csv
```

The host booted with persistent `zfs_qat_dc_max_buf_size=profile`, accepted a
manual concrete override, returned to `profile`, and rejected invalid values.
The benchmark harness smoke CSV recorded `128` aligned columns and included
both the stored value and the effective value:

```text
zfs_qat_dc_max_buf_size=profile
zfs_qat_dc_effective_max_buf_size=131072
zfs_qat_dc_profile_recordsize=131072
```

### Profile Compression Level

Run date: 2026-05-18.

The third profile implementation slice made the QAT DC compression level
profile-managed by default:

```text
zfs_qat_cpa_dc_level=profile|1|2|3|4
```

Effective behavior:

- `profile` uses `zfs_qat_dc_ratio_profile` to select the effective QAT
  compression level.
- `balanced` and `performance` resolve to level `1`, matching the previous best
  performance-biased candidate under concurrent work.
- `ratio` resolves to level `4`, preserving the highest-ratio behavior when the
  operator explicitly selects ratio preference.
- Concrete values remain manual overrides for this tunable only.
- Changes that would alter initialized QAT DC session compression level are
  rejected with `EBUSY`.

Validation:

```text
srcversion: 8AF72BE5032A7356514A5C4
/root/zfs-qat-profile-level-smoke-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-level-smoke-20260518.csv
```

The host booted with persistent `zfs_qat_cpa_dc_level=profile`. Before QAT DC
session initialization, the host accepted valid profile and manual values and
rejected invalid string and numeric values. After a QAT smoke workload
initialized DC sessions, the host rejected a profile change from effective level
`1` to level `4`, rejected manual level `4`, and accepted manual level `1`
because it did not require changing the active session level.

The benchmark harness smoke CSV recorded `129` aligned columns and included
both the stored value and the effective value:

```text
zfs_qat_cpa_dc_level=profile
zfs_qat_effective_cpa_dc_level=1
zfs_qat_dc_ratio_profile=balanced
```
