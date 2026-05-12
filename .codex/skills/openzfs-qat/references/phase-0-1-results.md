# Phase 0/1 Results

Run date: 2026-05-12.

Scope:

- Phase 0: baseline and re-baseline `pve.drewnet.online` DKMS source.
- Phase 1: make QAT 4.28 build integration reproducible for QAT 1.x work.

## Local Baseline

- Branch: `qat-usability-performance`.
- Starting commit: `8448c330656ff88d44cad0a0ef31fd8d3ef6b9f8`.
- Working tree was clean before phase 0/1 changes.

## Host Baseline

Host:

```text
pve.drewnet.online
kernel: 7.0.0-3-pve
QAT: qat_dev0, dh895xcc, state up
running ZFS: zfs-2.4.99-563_g5dd912192
```

Existing DKMS source:

```text
/usr/src/zfs-2.4.99
/var/lib/dkms/zfs/2.4.99/source -> /usr/src/zfs-2.4.99
```

The pre-existing host source was not a git checkout and contained stale experimental compression-level changes:

```text
zfs_qat_deflate_depth
qat_dc_comp_level_from_depth()
```

Those changes were not present in the local repository source and were removed by re-baselining.

Backup created before replacement:

```text
/root/zfs-2.4.99.pre-phase0.20260512T040517Z
/root/zfs-2.4.99.pre-phase0.latest -> /root/zfs-2.4.99.pre-phase0.20260512T040517Z
```

## Re-Baselined Source

The current local working tree was synced to:

```text
/root/openzfs-qat-src-stage
```

On the host, the staged tree was prepared with:

```bash
./autogen.sh
ICP_ROOT=/root/QAT/QAT.L.4.28.0-00004 scripts/dkms.mkconf -n zfs -v 2.4.99 -f dkms.conf
./configure --disable-dependency-tracking --prefix=/usr --with-config=kernel \
  --with-linux=/lib/modules/$(uname -r)/build \
  --with-linux-obj=/lib/modules/$(uname -r)/build \
  --with-qat=/root/QAT/QAT.L.4.28.0-00004
make distclean
```

The staged source then replaced:

```text
/usr/src/zfs-2.4.99
```

No ZFS module install, module reload, service restart, pool operation, or dracut package operation was performed.

## Code Changes

`config/kernel.m4` now:

- Requires the access-layer `Module.symvers` to be readable and contain `cpaDcGetNumInstances`, proving it has the `qat_api` CPA exports ZFS links against.
- Includes `quickassist/qat/Module.symvers` in `KBUILD_EXTRA_SYMBOLS` when present, which matches the QAT 4.28 tree on `pve.drewnet.online`.
- Emits a clearer configure error when the required access-layer symbols file is missing or wrong.

`scripts/dkms.mkconf` now:

- Preserves the existing no-QAT behavior when `ICP_ROOT` is unset during `dkms.conf` generation.
- When `ICP_ROOT` is set during `dkms.conf` generation, writes a default such as:

```text
ICP_ROOT="${ICP_ROOT:-/root/QAT/QAT.L.4.28.0-00004}"
```

This keeps later DKMS rebuilds QAT-enabled without hand-editing `dkms.conf`.

## QAT Configure Validation

On `pve.drewnet.online`, configure with QAT succeeded and selected both symbol files:

```text
checking qat source directory... /root/QAT/QAT.L.4.28.0-00004
checking qat build directory... /root/QAT/QAT.L.4.28.0-00004/build
checking qat file for module symbols... /root/QAT/QAT.L.4.28.0-00004/quickassist/lookaside/access_layer/src/Module.symvers /root/QAT/QAT.L.4.28.0-00004/quickassist/qat/Module.symvers
#define HAVE_QAT 1
```

The generated host DKMS config contains:

```text
ICP_ROOT="${ICP_ROOT:-/root/QAT/QAT.L.4.28.0-00004}"
```

## QAT DKMS Build Validation

Command:

```bash
dkms build -m zfs -v 2.4.99 -k $(uname -r) --force
```

Result:

```text
Building module(s)........... done.
Signing module /var/lib/dkms/zfs/2.4.99/build/module/zfs.ko
Signing module /var/lib/dkms/zfs/2.4.99/build/module/spl.ko
Running the post_build script... done.
```

Built module:

```text
/var/lib/dkms/zfs/2.4.99/7.0.0-3-pve/x86_64/module/zfs.ko
version: 2.4.99-1
depends: spl,qat_api
vermagic: 7.0.0-3-pve SMP preempt mod_unload modversions
```

Built config:

```text
#define HAVE_QAT 1
```

Important operational note:

- The built DKMS module differs from the currently installed/running module.
- This is expected because this phase intentionally built but did not install or reload modules.

Observed status:

```text
zfs/2.4.99, 7.0.0-3-pve, x86_64: installed (Original modules exist) (Differences between built and installed modules)
```

## Non-QAT Build Validation

An isolated temporary source copy was configured and built without `--with-qat`.

Result:

```text
/root/openzfs-noqat-build/module/zfs.ko
version: 2.4.99-1
depends: spl
vermagic: 7.0.0-3-pve SMP preempt mod_unload modversions
non-QAT build passed
```

This confirms the QAT changes do not force a QAT dependency when QAT is not requested.

## Remaining Notes

- Existing host package state includes `dracut-install 106-6`; this work did not install, remove, or modify dracut packages.
- Running ZFS remains `2.4.99-563_g5dd912192` from `/lib/modules/7.0.0-3-pve/updates/dkms/zfs.ko`.
- QAT kstats remained at zero because no workload was run in phase 0/1.
- Phase 2/3 should decide whether to install and load the newly built module or rebuild again after compression-level changes.
