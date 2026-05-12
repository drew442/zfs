# Phase 2/3 Results

Run date: 2026-05-12.

Scope:

- Phase 2: runtime enablement and observability.
- Phase 3: global QAT compression-level control for QAT 1.x.

## Source Baseline

Branch:

```text
qat-usability-performance
```

Starting commit:

```text
82aeb267508543270e2b0b6e67e3f6a4f480ed39
```

The abandoned `zfs_qat_deflate_depth` patch was not carried forward.

## Code Changes

`module/os/linux/zfs/qat_compress.c` now:

- Defines `zfs_qat_cpa_dc_level`, defaulting to `1`.
- Maps accepted values `1`, `2`, `3`, and `4` to `CPA_DC_L1`, `CPA_DC_L2`, `CPA_DC_L3`, and `CPA_DC_L4`.
- Uses that value when initializing QAT Deflate sessions instead of hardcoding `CPA_DC_L1`.
- Rejects invalid values with `EINVAL`.
- Rejects changes after QAT compression sessions initialize with `EBUSY`, while allowing the existing value to be written again.

`include/sys/qat.h` now declares the global compression-level parameter.

`man/man4/zfs.4` documents:

```text
zfs_qat_cpa_dc_level=1|2|3|4
```

The setting is global because the QAT compression level is a data-compression session setting, not a ZFS pool or dataset property.

## Host Deployment

Host:

```text
pve.drewnet.online
kernel: 7.0.0-3-pve
QAT: qat_dev0, dh895xcc, state up
```

Host DKMS source was re-baselined from the local repo before the phase 2/3 patch was built.

Backup created before replacement:

```text
/root/zfs-2.4.99.pre-phase2-3.20260512T060647Z
/root/zfs-2.4.99.pre-phase2-3.latest -> /root/zfs-2.4.99.pre-phase2-3.20260512T060647Z
```

The host modprobe configuration was changed to:

```text
options zfs zfs_qat_compress_disable=0 zfs_qat_checksum_disable=0 zfs_qat_cpa_dc_level=4
```

The obsolete `zfs_qat_deflate_depth` option was removed.

Install and boot flow:

```bash
dkms build -m zfs -v 2.4.99 -k $(uname -r) --force
dkms install -m zfs -v 2.4.99 -k $(uname -r) --force
update-initramfs -u -k $(uname -r)
reboot
```

No dracut package operation was performed.

## Build And Load Validation

Built and loaded module:

```text
/lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko
version: 2.4.99-1
depends: spl,qat_api
```

`modinfo zfs` shows QAT parameters including:

```text
zfs_qat_compress_disable
zfs_qat_cpa_dc_level
zfs_qat_checksum_disable
zfs_qat_encrypt_disable
```

`modinfo zfs` does not show the abandoned `zfs_qat_deflate_depth` parameter.

Runtime parameters after reboot:

```text
zfs_qat_checksum_disable=0
zfs_qat_compress_disable=0
zfs_qat_cpa_dc_level=4
zfs_qat_encrypt_disable=0
```

Pool health:

```text
zpool status -x
all pools are healthy
```

## Compression-Level Parameter Validation

After QAT compression sessions initialized at level `4`, invalid values were rejected:

```text
echo 5 > /sys/module/zfs/parameters/zfs_qat_cpa_dc_level
write error: Invalid argument
level remained 4
```

Changing the level after session initialization was rejected:

```text
echo 3 > /sys/module/zfs/parameters/zfs_qat_cpa_dc_level
write error: Device or resource busy
level remained 4
```

## QAT Offload Validation

Initial post-reboot workload:

- Dataset: `test-hdd-pool/bench/codex-phase3-qat-l4`.
- Properties: `compression=gzip-1`, `recordsize=128K`.
- Source: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`.
- Logical write size: about `183M`.
- SHA256 source and destination matched.
- Dataset compressratio: `16.86x`.
- QAT compression kstats remained at zero.

The zero kstats were caused by runtime initialization timing, not by the compression-level parameter. Re-writing `0` to the compression disable parameter after QAT was up triggered the existing lazy initialization path:

```bash
echo 0 > /sys/module/zfs/parameters/zfs_qat_compress_disable
```

Repeat workload:

- Dataset: `test-hdd-pool/bench/codex-phase3-retry`.
- Properties: `compression=gzip-1`, `recordsize=128K`.
- Source: `/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif`.
- Logical write size: `183M`.
- SHA256 source and destination matched.
- Dataset compressratio: `17.11x`.

QAT kstats before repeat workload:

```text
comp_requests=0
comp_total_in_bytes=0
comp_total_out_bytes=0
decomp_requests=0
dc_fails=0
```

QAT kstats after repeat workload:

```text
comp_requests=1460
comp_total_in_bytes=191365120
comp_total_out_bytes=6842026
decomp_requests=1404
decomp_total_in_bytes=10706944
decomp_total_out_bytes=184025088
dc_fails=0
```

Both test datasets were destroyed after validation.

## Remaining Notes

- The systemd boot log still shows an ordering cycle involving `qat.service` and ZFS import/mount units. This can leave ZFS loaded before QAT compression sessions are initialized.
- The existing lazy re-enable path works after QAT is up, but a future phase should decide whether to improve boot ordering, change the initialization retry policy, or document a post-boot enable step for this host.
- QAT logs show remote NUMA node messages because the accelerator is on node `2`; performance work should account for that.
- This phase proves correctness and observability for the global compression-level knob. It does not yet establish the best level or threshold for throughput.
