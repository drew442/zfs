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

## Follow-Up

- Continue phase 4 with throughput and latency as first-class requirements. Future benchmark output should include throughput, p50/p95/p99/max latency, CPU cost, compression ratio, QAT kstats, and failure counters.
- Continue larger-record benchmarking. Initial 256 KiB and 1 MiB validation proves QAT offload can work on this host, but the default should remain `128 KiB` until throughput and latency are compared against software gzip across the broader matrix.
- Evaluate optimization bias parameters only after measurements identify real policy choices. A throughput/latency bias such as `latency`, `balanced`, and `throughput` is useful if queueing, batching, thresholds, or reuse strategies create measured tradeoffs. A performance/ratio bias such as `performance`, `balanced`, and `compressionratio` is useful if compression effort or fallback policy creates measured tradeoffs.
- Keep explicit low-level parameters for benchmarking first. Bias parameters should later set coherent defaults across those low-level knobs; they should not be added as no-op labels before the policies are proven.
- Park NUMA performance tuning until a true multi-socket QAT 1.x host is available.
