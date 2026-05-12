# OpenZFS QAT Repo Map

## Primary files

- `config/kernel.m4`
  - `ZFS_AC_QAT` defines `--with-qat=PATH` and `--with-qat-obj=PATH`.
  - Build detection expects `${qatsrc}/quickassist/include/cpa.h`.
  - Object detection expects `icp_qa_al.ko` or `qat_api.ko`.
  - Symbol detection requires `lookaside/access_layer/src/Module.symvers` with CPA API exports and also includes `qat/Module.symvers` when present.
- `config/zfs-build.m4`
  - Carries the `CONFIG_QAT` automake conditional.
- `scripts/dkms.mkconf`
  - Propagates `--with-qat="${ICP_ROOT}"` into DKMS config generation.
  - When `ICP_ROOT` is set while generating `dkms.conf`, bakes that path in as the default for later DKMS rebuilds.
- `include/sys/qat.h`
  - Public QAT-facing header for this tree.
  - Declares compile-time gate `HAVE_QAT`, shared stats, disable tunables, accel predicates, and the public QAT entry points.
  - Defines the shared crypto/checksum offload window as `QAT_MIN_BUF_SIZE = 4 KiB` and `QAT_MAX_BUF_SIZE = 128 KiB`.
  - Defines the compression offload window as `QAT_DC_MIN_BUF_SIZE = 8 KiB` and `QAT_DC_MAX_BUF_SIZE = 128 KiB`.
- `module/os/linux/zfs/qat.c`
  - Shared initialization and teardown.
  - Creates the `zfs/qat` kstat set.
  - If init fails, the code disables the corresponding module parameters instead of failing module load.
- `module/os/linux/zfs/qat_compress.c`
  - Compression and decompression implementation.
  - Contains a fixed static cap: `QAT_DC_MAX_INSTANCES = 48`.
  - Owns `zfs_qat_compress_disable`, `zfs_qat_cpa_dc_level`, and the lazy re-enable path via the module parameter setter.
- `module/os/linux/zfs/qat_crypt.c`
  - AES-GCM encryption/decryption and SHA256 checksum offload.
  - Owns `zfs_qat_encrypt_disable` and `zfs_qat_checksum_disable`.
  - Lazy re-enable for crypto/checksum happens through the module parameter setters.
- `module/zfs/gzip.c`
  - Compression and decompression call sites. Falls back to software if QAT fails.
- `module/zfs/sha2_zfs.c`
  - SHA256 checksum call site. Falls back to software if QAT fails.
- `module/os/linux/zfs/zio_crypt.c`
  - AES-GCM call site. QAT is skipped for ZIL and dnode objects because the helper only supports in-place operation.
- `man/man4/zfs.4`
  - Documents `zfs_qat_checksum_disable`, `zfs_qat_compress_disable`, and `zfs_qat_encrypt_disable`.
- `contrib/intel_qat/`
  - Compatibility notes and patches for external Intel QAT driver builds.

## Current runtime controls

- `zfs_qat_compress_disable`
- `zfs_qat_cpa_dc_level`
- `zfs_qat_checksum_disable`
- `zfs_qat_encrypt_disable`

The disable flags are documented as disable flags, but setting them back to `0` also acts as a lazy initialization trigger when support was compiled in and the external QAT driver is present. `zfs_qat_cpa_dc_level` is a global QAT data-compression session setting and must be set before QAT compression initializes.

## Current behavioral constraints

- Compression offload is only considered within the `8 KiB` to `128 KiB` window from `include/sys/qat.h`.
- Crypto/checksum offload still uses the shared `4 KiB` to `128 KiB` window.
- Compression code uses a fixed maximum instance count of `48`.
- Encryption offload in `zio_crypt.c` is intentionally skipped for `DMU_OT_INTENT_LOG` and `DMU_OT_DNODE`.
- Most call sites attempt QAT first and then fall back to software if the accelerator path returns an error.

## Review checklist

- Build usability
  - Are `--with-qat` and `--with-qat-obj` assumptions still valid for modern QAT packaging?
  - Are configure errors specific enough to tell the user what is missing?
- Static configuration
  - Does a fixed cap like `QAT_DC_MAX_INSTANCES = 48` still make sense?
  - Are the compression-specific `8 KiB` to `128 KiB` thresholds defensible for current hardware and workloads?
- Runtime usability
  - Do the disable flags behave predictably when initialization fails once and later succeeds?
  - Is there enough visibility through kstats and module parameters?
- Performance
  - Are we paying too much in allocation, copying, or buffer setup for small requests?
  - Are failed offload attempts common enough that thresholds or heuristics should change?
- Correctness and fallback
  - Does every accelerated path preserve the existing software fallback?
  - If acceleration is skipped for a class of buffers, is that restriction documented in code or docs?

## Useful commands

Search the surface area:

```bash
rg -n "\bqat\b|QAT|qat_" .
```

Inspect the current branch diff once work starts:

```bash
git diff -- config/ include/ module/ man/ scripts/
```

Example configure flags for a QAT-enabled build:

```bash
./configure --with-qat=/path/to/QAT --with-qat-obj=/path/to/QAT/build
```

Example runtime toggles after module load:

```bash
cat /sys/module/zfs/parameters/zfs_qat_compress_disable
echo 0 | sudo tee /sys/module/zfs/parameters/zfs_qat_compress_disable
cat /sys/module/zfs/parameters/zfs_qat_checksum_disable
cat /sys/module/zfs/parameters/zfs_qat_encrypt_disable
```

Likely kstat location on Linux, inferred from `kstat_create("zfs", 0, "qat", ...)` in `module/os/linux/zfs/qat.c`:

```bash
cat /proc/spl/kstat/zfs/qat
```
