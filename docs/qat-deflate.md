# QAT Deflate Compression

This fork adds a real OpenZFS compression property value:

```sh
zfs set compression=qat-deflate pool/dataset
```

`qat-deflate` stores a distinct compression enum in block pointers and uses
the QAT deflate path when the kernel module is built with QAT support and QAT
is available for the block size. The QAT deflate compression depth is selected
globally with the `zfs_qat_deflate_depth` module parameter:

```sh
options zfs zfs_qat_compress_disable=0 zfs_qat_deflate_depth=16
```

Valid depth values are `1`, `4`, `8`, and `16`. For QAT 1.7/1.8 hardware these
map to `CPA_DC_L1`, `CPA_DC_L2`, `CPA_DC_L3`, and `CPA_DC_L4`.

## Compatibility

This is a fork-local on-disk compression type. Blocks written with
`compression=qat-deflate` require this fork, or another OpenZFS build with the
same compression enum, to read them. Use a disposable test pool until a proper
OpenZFS feature flag and compatibility story are implemented.

## Fallback Behavior

The compressor first attempts QAT in kernel builds with QAT enabled. If QAT is
unavailable, the block size is outside the QAT acceleration window, or a QAT
operation fails, compression falls back to software gzip-compatible deflate.
Decompression also falls back to software zlib if QAT cannot service the block.

## Block Size

The current QAT path only accelerates block sizes from 4 KiB through 128 KiB.
Use `recordsize=128K` for tests intended to exercise QAT compression.
