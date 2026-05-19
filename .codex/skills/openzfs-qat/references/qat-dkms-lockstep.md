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

## End-to-end rebuild flow

These commands are the known-good host flow used on `pve.drewnet.online`.
Run them from the OpenZFS repository checkout unless noted otherwise.

1. Update the OpenZFS checkout and QAT submodule:

```sh
git fetch drew qat-usability-performance
git checkout -B qat-usability-performance drew/qat-usability-performance
git submodule update --init contrib/qat/QAT.L.4.28.0-00004
```

2. Validate QAT DKMS installer prerequisites:

```sh
cd contrib/qat/QAT.L.4.28.0-00004
sudo QAT_DKMS_PREFLIGHT_ONLY=1 ./scripts/install-dkms.sh
```

3. Build and install the QAT DKMS package:

```sh
sudo QAT_DKMS_REPLACE=1 ./scripts/install-dkms.sh
```

4. Return to the OpenZFS checkout and generate the ZFS DKMS config:

```sh
cd /root/zfs
./autogen.sh
./scripts/dkms.mkconf -n zfs -v 2.4.99 -f dkms.conf
```

5. Refresh the ZFS DKMS source tree:

```sh
sudo rm -rf /usr/src/zfs-2.4.99.prev
if [ -d /usr/src/zfs-2.4.99 ]; then
    sudo mv /usr/src/zfs-2.4.99 /usr/src/zfs-2.4.99.prev
fi
sudo mkdir -p /usr/src/zfs-2.4.99
sudo rsync -a --delete \
    --exclude .git \
    --exclude 'contrib/qat/QAT.L.4.28.0-00004/.git' \
    ./ /usr/src/zfs-2.4.99/
```

6. Rebuild and reinstall ZFS DKMS against the QAT DKMS source path:

```sh
sudo dkms remove -m zfs -v 2.4.99 -k "$(uname -r)" || true
sudo dkms add -m zfs -v 2.4.99
sudo ICP_ROOT=/usr/src/qat-4.28.0-00004 dkms build --force -m zfs -v 2.4.99 -k "$(uname -r)"
sudo dkms install --force -m zfs -v 2.4.99 -k "$(uname -r)"
```

7. Update initramfs, install the optional QAT re-enable service, and reboot:

```sh
sudo update-initramfs -u -k "$(uname -r)"
sudo contrib/qat/install-zfs-qat-reenable.sh
sudo reboot
```

Do not install dracut packages on `pve.drewnet.online`; this flow is compatible with the host's existing initramfs-based boot path.

## Boot-order helper

If ZFS loads before `qat.service`, QAT compression remains disabled until
`zfs_qat_compress_disable` is toggled from `1` back to `0`. On
`pve.drewnet.online`, this is handled by
`/etc/systemd/system/zfs-qat-reenable.service`.

Install the optional helper from the repository with:

```sh
sudo contrib/qat/install-zfs-qat-reenable.sh
```

The helper is intentionally separate from DKMS packaging because it is a host
boot-order workaround, not a kernel module build requirement.

## Runtime validation

After reboot, verify QAT and ZFS both resolve to DKMS modules:

```sh
dkms status -m qat -v 4.28.0-00004
dkms status -m zfs -v 2.4.99
modinfo -n intel_qat
modinfo -n qat_api
modinfo -n usdm_drv
modinfo -n zfs
```

Expected module paths should resolve under `/lib/modules/$(uname -r)/updates/dkms`.

Verify QAT services and devices:

```sh
systemctl is-active qat.service
systemctl is-enabled zfs-qat-reenable.service
systemctl --no-pager --full status zfs-qat-reenable.service
adf_ctl status
```

Verify OpenZFS QAT runtime counters:

```sh
cat /sys/module/zfs/parameters/zfs_qat_compress_disable
awk '/dc_instances|dc_fails|comp_requests|comp_total_in_bytes|comp_total_out_bytes/ { print }' /proc/spl/kstat/zfs/qat
```

Verify QAT driver-side DC timing counters when the instrumented QAT DKMS build
is installed:

```sh
cat /proc/qat_dc_timing
cat /sys/kernel/debug/qat_api/dc_timing
```

Both files expose the same cumulative `name value` table from `qat_api.ko`.
Use before/after deltas around benchmark runs. If the disk module has been
replaced but the loaded `qat_api.ko` does not expose these files, compare
`cat /sys/module/qat_api/srcversion` with `modinfo -F srcversion qat_api`; an
initramfs refresh may be required.

A short smoke test should create a temporary gzip dataset, write a file, read it
back, compare the checksum, and confirm `comp_requests` increased with
`dc_fails=0`.
