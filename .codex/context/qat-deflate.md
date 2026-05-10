# QAT Deflate Design Context

Goals:

- Keep the existing `gzip.c` path untouched.
- Add a real `compression=qat-deflate` property value rather than an alias.
- Use the `zfs_qat_deflate_depth` module parameter for QAT hardware depth.
- Preserve software fallback for reads and writes when QAT is unavailable.

Important constraints:

- `qat-deflate` is a new compression enum and therefore has on-disk impact.
- The current implementation does not add an OpenZFS feature flag.
- QAT acceleration is currently limited to 4 KiB through 128 KiB blocks.
- `recordsize=128K` is the practical maximum for QAT-compressed datasets.

Validation targets:

- `zfs set compression=qat-deflate pool/dataset`
- `zfs get compression pool/dataset`
- write/read checksum correctness
- `/proc/spl/kstat/zfs/qat` compression request counters
- software fallback decompression after QAT service stop
