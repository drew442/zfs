# QAT DKMS Initramfs Recovery - 2026-05-23

Purpose: record the `pve.drewnet.online` boot failure caused by an incomplete
QAT DKMS rebuild and the recovery steps used to restore the normal
`7.0.0-3-pve` boot path.

## Failure

During the first `ICP_PARAM_CHECK=n` experiment, the QAT DKMS build failed but
the remote command continued through `update-initramfs` and reboot because the
pipeline used `tee` without `pipefail`.

The host dropped to an `(initramfs)` prompt. Console `dmesg` showed repeated
`qat_api` unknown-symbol and symbol-version mismatch messages when attempting
both:

```text
modprobe qat_api
modprobe zfs
```

The practical failure mode was a QAT/ZFS lock-step break:

- QAT DKMS had been removed and was only in `added` state.
- The initramfs had been regenerated with inconsistent QAT module state.
- `zfs.ko` expected QAT symbols that did not match the QAT modules available in
  the initramfs.

The host was recovered initially by booting the previous kernel:

```text
6.17.13-6-pve
```

## Root Cause

There were two issues:

- The first packaging implementation passed `--enable-param-check=n` to QAT
  `configure`, but QAT 4.28 expects `--enable-param-check=no`.
- The remote build command used `tee` without `set -o pipefail`, so the shell did
  not stop after `dkms build` failed.

The QAT submodule now maps:

```text
QAT_DKMS_PARAM_CHECK=y -> --enable-param-check=yes
QAT_DKMS_PARAM_CHECK=n -> --enable-param-check=no
```

## Recovery

From the working `6.17.13-6-pve` boot, QAT and ZFS were rebuilt explicitly for
the target kernel:

```text
TARGET_KERNEL=7.0.0-3-pve
QAT_USR=/usr/src/qat-4.28.0-00004
```

Recovery steps:

```text
1. Remove the incomplete qat/4.28.0-00004 DKMS state.
2. Refresh /usr/src/qat-4.28.0-00004 from the QAT source tree.
3. Build and install qat/4.28.0-00004 for 7.0.0-3-pve.
4. Confirm all managed QAT modules resolve from /updates/dkms for 7.0.0-3-pve.
5. Remove, rebuild, and reinstall zfs/2.4.99 for 7.0.0-3-pve with ICP_ROOT=/usr/src/qat-4.28.0-00004.
6. Regenerate initramfs for 7.0.0-3-pve.
7. Reboot into 7.0.0-3-pve and verify QAT/ZFS health.
```

Artifact:

```text
.codex/skills/openzfs-qat/artifacts/repair-qat-zfs-7.0.0-3-pve-20260522.log
```

## Post-Recovery State

After reboot:

```text
kernel: 7.0.0-3-pve
qat.service: active
qat_dev0: dh895xcc, state up
qat_dev1: dh895xcc, state up
qat/4.28.0-00004, 7.0.0-3-pve, x86_64: installed
zfs/2.4.99, 7.0.0-3-pve, x86_64: installed
```

All managed QAT modules resolved from `/lib/modules/7.0.0-3-pve/updates/dkms/`:

```text
intel_qat
qat_api
usdm_drv
qat_dh895xcc
qat_c62x
qat_dh895xccvf
qat_c62xvf
```

ZFS also resolved from DKMS:

```text
zfs: /lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko
spl: /lib/modules/7.0.0-3-pve/updates/dkms/spl.ko
```

Runtime QAT state:

```text
zfs_qat_dc_profile=balanced
zfs_qat_dc_profile_recordsize=131072
zfs_qat_dc_max_buf_size=profile
zfs_qat_compress_disable=0
zfs_qat_checksum_disable=1
zfs_qat_encrypt_disable=1
zfs_qat_cpa_dc_level=profile
zfs_qat_decompress_disable=profile
dc_instances=12
dc_watchdog_health=1
dc_watchdog_runtime_disables=0
dc_fails=0
```

## Smoke Test

A temporary dataset was created on `test-hdd-pool` with `compression=gzip-1` and
`recordsize=128K`. A deterministic payload was written, synced, read back, and
verified with `sha256sum`.

Result:

```text
/test-hdd-pool/qat-repair-smoke-20260523/payload.bin: OK
comp_requests: 0 -> 1920
comp_total_in_bytes: 0 -> 251658240
comp_total_out_bytes: 0 -> 1336320
dc_fails: 0 -> 0
dc_instances: 12
dc_watchdog_health: 1
```

The temporary dataset was destroyed after the smoke test.

## Rules For Future Experiments

- Use `bash -o pipefail` for any remote command that pipes `dkms`, `make`, or
  `update-initramfs` output through `tee`.
- Do not run `update-initramfs` after a failed QAT or ZFS DKMS build.
- When booted into a rescue kernel, build QAT and ZFS explicitly for the target
  kernel with `dkms build -k <target-kernel>` and `dkms install -k
  <target-kernel>`.
- Rebuild ZFS after replacing QAT DKMS so `zfs.ko` and `qat_api.ko` remain
  symbol-compatible in both `/lib/modules` and initramfs.
- Verify `modinfo -k <target-kernel> -n qat_api` and `modinfo -k
  <target-kernel> -n zfs` both resolve under `/updates/dkms` before regenerating
  initramfs.
