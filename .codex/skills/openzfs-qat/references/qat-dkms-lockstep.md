# QAT DKMS lock-step plan

Purpose: keep the out-of-tree QAT 4.28 driver source and ZFS DKMS QAT build input aligned for QAT 1.x development.

## Source layout

- QAT submodule: `contrib/qat/QAT.L.4.28.0-00004`
- QAT development branch: `codex/openzfs-qat-submodule`
- Upstream source package version: `QAT.L.4.28.0-00004`
- DKMS package identity: `qat/4.28.0-00004`
- DKMS source path: `/usr/src/qat-4.28.0-00004`

## Initial DKMS scope

The DKMS configuration installs the QAT common module, kernel API module, USDM module, and the QAT 1.x dh895xcc/C62x physical and VF modules:

- `intel_qat`
- `qat_api`
- `usdm_drv`
- `qat_dh895xcc`
- `qat_c62x`
- `qat_dh895xccvf`
- `qat_c62xvf`

The upstream package can build additional device modules, including QAT 2.0+/Gen4 modules, but those are not installed by this project DKMS configuration because they are outside the current hardware scope.

## ZFS DKMS integration

`scripts/dkms.mkconf` now emits:

```sh
ICP_ROOT="${ICP_ROOT:-/usr/src/qat-4.28.0-00004}"
```

This means generated ZFS `dkms.conf` files use the QAT DKMS source path for `--with-qat` unless an operator overrides `ICP_ROOT` or sets `ZFS_DKMS_QAT_ICP_ROOT` while generating the ZFS DKMS config.

Install or rebuild QAT DKMS before rebuilding ZFS DKMS. The ZFS configure checks need the QAT headers, built objects under `${ICP_ROOT}/build`, and QAT `Module.symvers` files to come from the same QAT source tree and target kernel.

## Host deployment order

1. Build and install the QAT DKMS package from `contrib/qat/QAT.L.4.28.0-00004`.
2. Confirm `/usr/src/qat-4.28.0-00004/build/qat_api.ko` exists for the target kernel build.
3. Rebuild the ZFS DKMS package so `--with-qat=/usr/src/qat-4.28.0-00004` is used.
4. Verify `modinfo zfs` shows the expected QAT dependency and `/proc/spl/kstat/zfs/qat` appears after module load.

Do not install dracut packages on `pve.drewnet.online`; this flow is compatible with the host's existing initramfs-based boot path.
