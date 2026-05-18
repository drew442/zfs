# pve.drewnet.online QAT/ZFS Host Notes

Last inspected: 2026-05-12 via SSH as `root@pve.drewnet.online`.
Operator context updated: 2026-05-12.

These notes document the observed QAT-enabled OpenZFS deployment on the Proxmox host. Machine IDs, product UUIDs, serial numbers, and full disk identifiers are intentionally omitted.

## Project Role And Safety Rules

- `pve.drewnet.online` is designated specifically for this OpenZFS/QAT project.
- The host contains no production data and no real production copy.
- The `nvme_scratch` pool contains real-world test data only.
- The `test-hdd-pool` pool is a performance-testing pool.
- It is acceptable to use this host for required project testing, including controlled write workloads and benchmarks.
- The host boots with initramfs. Do not install dracut packages that ZFS, DKMS, packaging tools, or dependency resolution may create or suggest.
- Before changing `/usr/src/zfs-2.4.99/`, verify whether it needs to be re-baselined because earlier experimental patches may remain there.
- Treat `/root/` benchmark scripts and CSVs as project artifacts, not production data.

## Access

```bash
ssh root@pve.drewnet.online
```

Connectivity was verified with non-interactive SSH:

```bash
ssh -o BatchMode=yes -o ConnectTimeout=8 root@pve.drewnet.online hostname -f
```

Observed hostname: `pve.drewnet.online`.

## Platform

- OS: Debian GNU/Linux 13 `trixie`.
- Proxmox: `proxmox-ve` 9.1.0, `pve-manager` 9.1.9.
- Kernel: `7.0.0-3-pve` built 2026-04-21.
- Hardware model: Gigabyte `MZ31-AR0-00`.
- CPU: AMD EPYC 7551P, 32 cores / 64 threads.
- Memory: about 110 GiB.
- NUMA: 4 nodes.
- QAT PCI device is on NUMA node `2`.

Relevant commands:

```bash
hostnamectl
cat /etc/os-release
uname -a
pveversion -v
lscpu
cat /sys/bus/pci/devices/0000:45:00.0/numa_node
```

## QAT Hardware

Two Intel QAT accelerators are present and up:

```text
0000:45:00.0 Intel DH895XCC Series QAT [8086:0435]
0000:64:00.0 Intel DH895XCC Series QAT [8086:0435]
Kernel driver: dh895xcc
Kernel module: qat_dh895xcc
```

`adf_ctl status` reports:

```text
qat_dev0 - type: dh895xcc, inst_id: 0, node_id: 2, bsf: 0000:45:00.0, #accel: 6 #engines: 12 state: up
qat_dev1 - type: dh895xcc, inst_id: 1, node_id: 3, bsf: 0000:64:00.0, #accel: 6 #engines: 12 state: up
```

Loaded QAT-related modules:

```text
usdm_drv
qat_dh895xcc
qat_api
intel_qat
```

Relevant commands:

```bash
lspci -nnk | grep -iA4 -E 'quickassist|qat|co-processor|crypto'
lsmod | egrep '^(qat|intel_qat|usdm|zfs|spl)'
adf_ctl status
```

## QAT Software

Installed source/build tree:

```text
/root/QAT/QAT.L.4.28.0-00004
```

Operator-provided source origin:

```text
https://github.com/drew442/QAT.L.4.28.0-00004.git
```

This is a custom QAT 4.28 tree modified to compile against the currently running Proxmox kernel. Do not assume it is byte-identical to Intel's published `QAT.L.4.28.0-00004` package.

The Intel systemd unit and source headers identify the deployed driver as:

```text
QAT.L.4.28.0-00004
```

Important files observed:

```text
/root/QAT/QAT.L.4.28.0-00004/quickassist/include/cpa.h
/root/QAT/QAT.L.4.28.0-00004/quickassist/qat/Module.symvers
/root/QAT/QAT.L.4.28.0-00004/build/qat_api.ko
/root/QAT/QAT.L.4.28.0-00004/build/intel_qat.ko
/root/QAT/QAT.L.4.28.0-00004/build/qat_dh895xcc.ko
/root/QAT/QAT.L.4.28.0-00004/build/usdm_drv.ko
```

QAT service:

```text
Unit: qat.service
Loaded from: /etc/systemd/system/qat.service
ExecStart: /etc/init.d/qat_service start
State observed: active (exited) when started successfully
Enabled: yes
```

`/etc/default/qat` contains:

```text
ENABLE_KAPI=1
```

SR-IOV is not enabled in `/etc/default/qat`.

Relevant commands:

```bash
systemctl status qat.service --no-pager
cat /etc/default/qat
find /root/QAT/QAT.L.4.28.0-00004 -maxdepth 3 -type f \
  \( -name cpa.h -o -name Module.symvers -o -name '*.ko' \)
```

## QAT Runtime Configuration

Active config files:

```text
/etc/dh895xcc_dev0.conf
/etc/dh895xcc_dev1.conf
```

Observed non-comment service and instance settings:

```text
[GENERAL]
ServicesEnabled = cy;dc

[KERNEL]
NumberCyInstances = 1
NumberDcInstances = 0
Cy0Name = "IPSec0"
Cy0IsPolled = 0
Cy0CoreAffinity = 0

[KERNEL_QAT]
NumberCyInstances = 0
NumberDcInstances = 6
Dc0Name = "IPComp0"
Dc0IsPolled = 0
Dc0CoreAffinity = 1
Dc1Name = "IPComp1"
Dc1IsPolled = 0
Dc1CoreAffinity = 2
Dc2Name = "IPComp2"
Dc2IsPolled = 0
Dc2CoreAffinity = 3
Dc3Name = "IPComp3"
Dc3IsPolled = 0
Dc3CoreAffinity = 4
Dc4Name = "IPComp4"
Dc4IsPolled = 0
Dc4CoreAffinity = 5
Dc5Name = "IPComp5"
Dc5IsPolled = 0
Dc5CoreAffinity = 6

[SSL]
NumberCyInstances = 2
NumberDcInstances = 2
NumProcesses = 1
LimitDevAccess = 0
```

Performance experiment note: a temporary `[KERNEL_QAT]` split of
`NumberCyInstances = 2` and `NumberDcInstances = 4` was tested on 2026-05-13.
The result was mixed, so the host was restored to the original
`NumberCyInstances = 4` and `NumberDcInstances = 2` split.

Current project experiment note: on 2026-05-17 the host was switched to a
DC-only `[KERNEL_QAT]` split with `NumberCyInstances = 0` and
`NumberDcInstances = 6`. The QAT 1.x driver accepted the configuration after
reboot. The previous config was backed up as:

```text
/etc/dh895xcc_dev0.conf.pre-dc6-20260517T013612Z
/etc/dh895xcc_dev0.conf.pre-dc6.latest -> /etc/dh895xcc_dev0.conf.pre-dc6-20260517T013612Z
```

Second-card scale note: on 2026-05-18 `/etc/dh895xcc_dev1.conf` was created by
copying the DC-only `/etc/dh895xcc_dev0.conf`, then the host was rebooted so
ZFS QAT DC initialization could see both cards. The active scale configuration
is two DH895XCC devices with `12` total `[KERNEL_QAT]` DC instances and `0`
total `[KERNEL_QAT]` crypto instances.

The ZFS QAT kstat now includes `dc_instances`, which records the active DC
instances initialized by ZFS after the lazy QAT DC init path runs. Immediately
after boot this can remain `0` until QAT compression is first exercised; use it
as ZFS-path observability, not as a replacement for `adf_ctl status`.

Observation: `adf_ctl` and the kernel can report the device up while ZFS QAT kstats remain at zero. Do not infer from driver state alone that ZFS has processed QAT-accelerated I/O.

Boot ordering caveat observed on 2026-05-13: systemd may delete the
`qat.service` start job to break a ZFS import ordering cycle. In that state
`adf_ctl status` can still report `qat_dev0` as up, but ZFS QAT compression may
not be initialized and benchmark runs will show `comp_requests=0`. If this
happens, start QAT explicitly and re-enable ZFS QAT compression:

```bash
systemctl start qat
echo 1 > /sys/module/zfs/parameters/zfs_qat_compress_disable
echo 0 > /sys/module/zfs/parameters/zfs_qat_compress_disable
```

Then confirm a QAT-mode benchmark moves `/proc/spl/kstat/zfs/qat` counters.

Relevant commands:

```bash
sed -e 's/#.*//' -e '/^[[:space:]]*$/d' /etc/dh895xcc_dev0.conf
adf_ctl status
cat /proc/spl/kstat/zfs/qat
```

## OpenZFS Deployment

ZFS userspace:

```text
openzfs-zfsutils 2.4.99-1
```

Residual package config also exists for Proxmox `zfsutils-linux 2.3.4-pve1`.

ZFS versions:

```text
zfs-2.4.99-563_g5dd912192
zfs-kmod-2.4.99-1
```

DKMS status:

```text
zfs/2.4.99, 7.0.0-3-pve, x86_64: installed (Original modules exist)
```

Loaded module before the phase 2/3 install pass:

```text
/lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko
version: 2.4.99-563_g5dd912192
depends: spl,qat_api
vermagic: 7.0.0-3-pve SMP preempt mod_unload modversions
```

Loaded module after the phase 2/3 install pass:

```text
filename: /lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko
version: 2.4.99-1
depends: spl,qat_api
```

DKMS source/config evidence:

```text
/usr/src/zfs-2.4.99
/var/lib/dkms/zfs/2.4.99/source/dkms.conf
/var/lib/dkms/zfs/2.4.99/7.0.0-3-pve/x86_64/log/make.log
```

Operator note: `/usr/src/zfs-2.4.99/` may contain experimental patches from earlier attempts that are no longer relevant. Re-baseline the DKMS source tree before treating it as clean source for new patches or performance conclusions.

The DKMS config defaults `ICP_ROOT` to:

```text
/root/QAT/QAT.L.4.28.0-00004
```

The DKMS build log shows configure ran with:

```text
--with-qat=/root/QAT/QAT.L.4.28.0-00004
```

The built `zfs_config.h` contains:

```c
#define HAVE_QAT 1
```

Relevant commands:

```bash
zfs version
dkms status
modinfo zfs | egrep '^(filename|version|srcversion|vermagic|depends|parm: zfs_qat)'
grep -n 'with-qat' /var/lib/dkms/zfs/2.4.99/7.0.0-3-pve/x86_64/log/make.log
grep HAVE_QAT /var/lib/dkms/zfs/2.4.99/7.0.0-3-pve/x86_64/zfs_config.h
```

## ZFS QAT Runtime State

Observed module parameters:

```text
zfs_qat_checksum_disable=1
zfs_qat_compress_disable=0
zfs_qat_decompress_disable=0
zfs_qat_cpa_dc_level=4
zfs_qat_dc_max_buf_size=1048576
zfs_qat_dc_max_instances=48
zfs_qat_encrypt_disable=1
zfs_qat_cy_max_instances=48
```

`/etc/modprobe.d/zfs-qat.conf` contains:

```text
options zfs zfs_qat_compress_disable=0 zfs_qat_checksum_disable=1 zfs_qat_encrypt_disable=1 zfs_qat_cpa_dc_level=4 zfs_qat_dc_max_buf_size=1048576
```

The previous modprobe config was backed up as:

```text
/etc/modprobe.d/zfs-qat.conf.pre-dc6-20260517T013612Z
/etc/modprobe.d/zfs-qat.conf.pre-dc6.latest -> /etc/modprobe.d/zfs-qat.conf.pre-dc6-20260517T013612Z
```

Historical note:

- `zfs_qat_deflate_depth` was part of an abandoned experimental patch and should not be carried forward.
- Current work should use `zfs_qat_cpa_dc_level` for the global QAT compression level.
- `zfs_qat_deflate_depth` was removed from the host modprobe configuration during the phase 2/3 pass.
- `zfs_qat_decompress_disable=1` disables QAT gzip decompression while leaving QAT gzip compression eligible. Phase 4 testing showed this can improve some QAT-write readback workloads, especially the four-job test, but it did not make QAT faster than full software gzip and the single-job result was mixed.
- Phase 4 in-flight counters showed QAT compression is not limited to one request at a time: the corrected v2 test reached peak `dc_compress_inflight_max=25` with one copy stream and `50` with four copy streams.
- `zfs_qat_cpa_dc_hufftype=static` was tested on 2026-05-14. It was functional, but the result was mixed and compression ratio dropped; the host boot configuration was restored to the default `dynamic` behavior.
- Compression-bound sizing with `cpaDcDeflateCompressBound()` was installed on 2026-05-14. It reduced additional compression scratch bytes by about 71% in the tested matrix, with zero QAT overflows; elapsed performance was mixed.
- `zfs_qat_dc_coalesce_src=1` was tested on 2026-05-14. It reduced QAT compression source buffers to 1 and improved 128K/256K in the tested matrix, but 1M regressed; the host was restored to the default `0`.
- `zfs_qat_dc_coalesce_dst=1` was tested on 2026-05-15. It reduced destination plus scratch output buffers to one QAT buffer; single-job results were mixed, and four-job results improved modestly. The host was restored to the default `0`.
- Destination coalescing reuse was tested on 2026-05-15 with 32 reusable slots per DC instance. It reduced allocation cost after warmup, but elapsed results remained mixed and four-job runs regressed; the host was restored to `zfs_qat_dc_coalesce_dst=0`.
- `zfs_qat_cpa_dc_level=1..4` was tested on 2026-05-15 with dynamic Huffman and coalescing disabled. All tested levels completed with zero DC failures. Level 4 gave the best compression ratio, level 1 was generally strongest under four concurrent jobs, and level 3 was fastest for single-job 128K and 1M in that run. The host boot configuration was restored to level 4 after the matrix.
- A best-case `zfs_qat_cpa_dc_level=1` QAT-vs-software comparison was run on 2026-05-16 with software readback verification. QAT was 1.0% faster only at single-job 128K; software remained faster for 256K/1M and under four jobs. The host boot and runtime configuration was restored to level 4 after the comparison.
- `/nvme_scratch` was recreated empty on 2026-05-15. The TIFF benchmark source was restored from `test-hdd-pool/bench/cpu-lz4/realdata-test/2021-09-05/Scanned Documents/Image.tif` to `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`.

Initial QAT kstats before the phase 2/3 validation workload:

```text
comp_requests=0
decomp_requests=0
dc_fails=0
encrypt_requests=0
decrypt_requests=0
crypt_fails=0
cksum_requests=0
cksum_fails=0
```

Interpretation: QAT support is compiled in and runtime enable flags are set to enabled, but the observed kstats do not prove that QAT has accelerated any ZFS workload since module load.

Phase 2/3 validation showed a boot-order caveat: after reboot, the first controlled gzip workload did not move QAT compression kstats. Re-writing `0` to `/sys/module/zfs/parameters/zfs_qat_compress_disable` after `qat.service` was up triggered the lazy initialization path, and a repeat gzip workload moved `comp_requests` from `0` to `1460` with `dc_fails=0`.

Operational check after boot:

```bash
echo 0 > /sys/module/zfs/parameters/zfs_qat_compress_disable
cat /proc/spl/kstat/zfs/qat
```

Relevant commands:

```bash
cat /sys/module/zfs/parameters/zfs_qat_compress_disable
cat /sys/module/zfs/parameters/zfs_qat_cpa_dc_level
cat /sys/module/zfs/parameters/zfs_qat_checksum_disable
cat /sys/module/zfs/parameters/zfs_qat_encrypt_disable
ls -l /sys/module/zfs/parameters | grep -E 'qat|deflate'
cat /proc/spl/kstat/zfs/qat
cat /etc/modprobe.d/zfs-qat.conf
```

## Pool Layout

Pools observed:

- `rpool`: single SATA SSD root pool, online. `zpool status` reports supported/requested features not yet enabled.
- `nvme_scratch`: striped pool using two NVMe partitions, online. Contains real-world testing data, not production data.
- `test-hdd-pool`: three-HDD `raidz1` data vdev, plus mirrored NVMe special vdev and mirrored NVMe SLOG, online. Dedicated to performance testing with and without QAT.

Dataset compression/encryption summary:

- `rpool/*`: compression `on`, encryption `off`.
- `nvme_scratch/*`: compression `off`, encryption `off`.
- `test-hdd-pool`: compression `lz4`, encryption `off`.
- `test-hdd-pool/bench/cpu-lz4`: compression `lz4`.
- `test-hdd-pool/bench/qat-gzip-1`: compression `gzip-1`.
- `test-hdd-pool/bench/qat-gzip-2`: compression `gzip-2`.
- `test-hdd-pool/bench/qat-gzip-3`: compression `gzip-3`.
- `test-hdd-pool/bench/qat-gzip-4`: compression `gzip-4`.

Relevant commands:

```bash
zpool status -v
zpool list -v
zfs list -o name,used,avail,refer,mountpoint,compression,compressratio,encryption,keylocation -t filesystem,volume
```

## Boot And Service Notes

Boot log observations:

- ZFS root is booted with `root=ZFS=rpool/ROOT/pve-1 boot=zfs`.
- `spl` and `zfs` are out-of-tree modules and taint the kernel, expected for DKMS OpenZFS.
- QAT device `qat_dev0` starts successfully with 12 acceleration engines.
- Several QAT messages report the device on remote NUMA node `2` differing from application nodes `0`, `1`, or `3`.
- systemd reported an ordering cycle involving `local-fs.target`, `zfs-mount.service`, `zfs-import-cache.service`, and `qat.service`; it deleted `zfs-mount.service/start` to break the cycle.

Observed ZFS services:

```text
zfs-import-cache.service active
zfs-share.service active
zfs-volume-wait.service active
zfs-zed.service active
zfs-mount.service inactive
```

Relevant commands:

```bash
dmesg --ctime | egrep -i 'qat|quickassist|zfs|spl'
journalctl -b -u qat.service --no-pager
systemctl --no-pager --type=service | grep -E 'zfs|zed'
```

## Local Artifacts On Host

The root home directory contains QAT/ZFS benchmark scripts and CSV outputs. These scripts were used for performance testing with and without QAT before the static `DEPTH_1` / `CPA_DC_L1` compression-level behavior was identified in the ZFS source.

```text
/root/zfs-qat-bench-realdata-read-v1.sh
/root/zfs-qat-bench-realdata-read-v2.sh
/root/zfs-qat-bench-realdata-v2.sh
/root/zfs-qat-bench-realworld-v2.sh
/root/zfs-qat-bench-realworld.sh
/root/qat-phase4-benchmark.sh
/root/zfs-qat-*.csv
```

Recent phase 4 policy benchmark artifacts:

```text
/root/zfs-qat-phase4-async-dc6-policy-fixed-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-policy-recordsize-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-policy-fixed-1m-repeat-jobs4-20260517.csv
/root/zfs-qat-phase4-async-dc6-policy-recordsize-1m-repeat-jobs4-20260517.csv
/root/zfs-qat-phase4-async-src-coalesce-smoke-20260517.csv
/root/zfs-qat-phase4-async-dst-coalesce-smoke-20260517.csv
/root/zfs-qat-phase4-async-coalesce-off-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-src-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-dst-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-both-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-off-focus-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-src-focus-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-dst-focus-jobs4-20260517.csv
/root/zfs-qat-phase4-async-coalesce-both-focus-jobs4-20260517.csv
/root/zfs-qat-phase4-methodology-current-policy-20260518.csv
/root/zfs-qat-scale-single-card-jobs4-20260518.csv
/root/zfs-qat-scale-single-card-jobs8-20260518.csv
/root/zfs-qat-scale-dual-card-jobs4-20260518.csv
/root/zfs-qat-scale-dual-card-jobs8-20260518.csv
/root/zfs-qat-scale-dual-card-perinst-policy-jobs4-20260518.csv
/root/zfs-qat-scale-dual-card-perinst-policy-jobs8-20260518.csv
/root/zfs-qat-profile-cap-128k-cap1024-jobs8-20260518.csv
/root/zfs-qat-profile-cap-128k-cap1280-jobs8-20260518.csv
/root/zfs-qat-profile-cap-256k-cap256-jobs8-20260518.csv
/root/zfs-qat-profile-cap-256k-cap320-jobs8-20260518.csv
/root/zfs-qat-profile-cap-1m-cap128-jobs8-20260518.csv
/root/zfs-qat-profile-cap-1m-cap160-jobs8-20260518.csv
```

Current QAT async policy parameters after the 2026-05-18 methodology benchmark
restore:

```text
zfs_qat_dc_async=0
zfs_qat_dc_async_max_inflight=96
zfs_qat_dc_async_cap_policy=fixed
zfs_qat_dc_coalesce_src=0
zfs_qat_dc_coalesce_dst=0
zfs_qat_decompress_disable=0
```

The installed ZFS module includes `zfs_qat_dc_async_cap_policy`,
`dc_instances` kstat observability, and conservative active-DC cap calculation.
It reported `srcversion 3113C7A062DD66A0FCD30A7` after the 2026-05-18
forced DC-count policy DKMS rebuild, initramfs update, and reboot. The live host
rejected invalid cap-policy values with `EINVAL` and accepted both `fixed` and
`recordsize`.

Recent DC-count policy build artifacts:

```text
/root/zfs-qat-dc-count-policy-dkms-build-20260518.log
/root/zfs-qat-dc-count-policy-dkms-install-20260518.log
/root/zfs-qat-dc-count-policy-initramfs-20260518.log
/root/zfs-qat-dc-count-policy-dkms-build-20260518-r2.log
/root/zfs-qat-dc-count-policy-dkms-install-20260518-r2.log
/root/zfs-qat-dc-count-policy-initramfs-20260518-r2.log
/root/zfs-qat-dc-count-policy-dkms-build-20260518-r3.log
/root/zfs-qat-dc-count-policy-dkms-install-20260518-r3.log
/root/zfs-qat-dc-count-policy-initramfs-20260518-r3.log
```

The async-compatible coalescing DKMS build reported
`srcversion 18635F01D8EFD4EBD6C7675` after reboot. Build artifacts:

```text
/root/zfs-2.4.99.pre-async-coalesce.20260517T072844Z
/root/zfs-qat-async-coalesce-dkms-build-20260517.log
/root/zfs-qat-async-coalesce-dkms-install-20260517.log
/root/zfs-qat-async-coalesce-initramfs-20260517.log
```

QATzip source is present under:

```text
/root/qatzip/QATzip
```

No `qzip` binary was found in `PATH` during inspection. `ldconfig` shows QAT libraries including:

```text
/usr/local/lib/libqat_s.so
/lib/x86_64-linux-gnu/libqat.so.4
```

## Follow-Up Checks

- Review the systemd ordering cycle if QAT should initialize before ZFS import/mount without a manual lazy re-enable.
- Run controlled gzip workloads and compare `/proc/spl/kstat/zfs/qat` before/after to prove actual ZFS QAT offload for each benchmark.
- Review NUMA placement if benchmarking QAT, because the QAT device is on node `2` and logs show remote-node access.
- Consider whether `rpool` should remain feature-lagged or be upgraded; this is operationally separate from QAT.
