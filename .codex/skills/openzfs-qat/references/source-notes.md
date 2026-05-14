# OpenZFS QAT Source Notes

These notes capture stable, primary-source details useful when reviewing this fork's QAT integration. Keep source-derived facts separate from community observations. Re-check external docs before hard-coding version-specific claims.

## Hardware Scope

- Project target: QAT 1.x hardware, specifically dh895x/dh895xcc and C620/C62x-class cards.
- Out of scope: features, APIs, driver paths, or performance guidance that require QAT hardware version 2.0 or newer.
- Treat QAT 2.0, Gen4, SVM, and in-tree-driver-only guidance as background unless the same behavior is documented for QAT 1.x and works with the out-of-tree CE driver used here.

## Source Priority

- Primary: this repository's code, Intel QAT documentation, Intel driver release/download pages, and OpenZFS upstream code/discussions when they describe the implementation directly.
- Secondary: TrueNAS/forum/gist/blog material. Use these as discovery leads only; do not copy opinions, unsupported hardware claims, or performance conclusions into code or docs without verifying against primary sources.
- Research: the QZFS USENIX ATC 2019 paper is useful background for design intent, but this fork's implementation and current Intel API docs are the source of truth for behavior.

## Intel Documentation Links

- QAT overview and resource hub: https://www.intel.com/content/www/us/en/developer/topic-technology/open/quick-assist-technology/overview.html
- QAT documentation hub: https://intel.github.io/quickassist/index.html
- QAT CE Linux driver download page: https://www.intel.com/content/www/us/en/download/19734/intel-quickassist-technology-intel-qat-driver-for-linux-for-customer-enabling-ce-release.html
- Getting Started / installation guide: https://intel.github.io/quickassist/GSG/2.X/installation.html
- Programmer's Guide, in-tree vs out-of-tree: https://intel.github.io/quickassist/PG/in_tree_vs_oot.html
- Programmer's Guide, memory management: https://intel.github.io/quickassist/PG/infrastructure_memory_management.html
- Programmer's Guide, logical instances: https://intel.github.io/quickassist/PG/configuration_files_logicalsection.html
- Programmer's Guide, compression API: https://intel.github.io/quickassist/PG/services_compression_api.html
- Programmer's Guide, crypto API: https://intel.github.io/quickassist/PG/services_cryptography_api.html
- Performance guide: https://intel.github.io/quickassist/PERF/design_guidelines.html and https://intel.github.io/quickassist/PERF/application_tuning.html

## Build And Driver Assumptions

- This tree's `config/kernel.m4` expects the external driver source under `ICP_ROOT` to expose `quickassist/include/cpa.h`.
- The same configure logic expects build objects under `--with-qat-obj`, defaulting to `$ICP_ROOT/build`, and accepts either `icp_qa_al.ko` or `qat_api.ko`.
- QAT symbol detection needs the access-layer `Module.symvers` containing `qat_api` CPA exports such as `cpaDcGetNumInstances`; QAT 4.28 may also provide `quickassist/qat/Module.symvers` for driver symbols.
- `scripts/dkms.mkconf` passes `--with-qat="${ICP_ROOT}"`; when `ICP_ROOT` is set during `dkms.conf` generation, the generated file keeps that path as the default for later DKMS rebuilds.
- Intel documents `--enable-kapi` as enabling the Intel QuickAssist API in kernel space. Treat this as relevant to OpenZFS direct kernel API integration when building the out-of-tree driver.
- Intel documents `--enable-icp-sriov` with `host` and `guest` modes. SR-IOV setup claims from community posts should be verified against Intel virtualization docs and the actual hardware/driver generation.
- Intel's current docs distinguish in-tree and out-of-tree stacks and state that QAT Gen4 and later development is moving toward in-tree drivers. Gen4/in-tree-only behavior is outside this project's target unless it also applies to QAT 1.x.
- Intel's installation guide lists platform configuration filenames such as `4xxx_dev0.conf`, `dh895xcc_dev0.conf`, `c6xx_dev0.conf`, `c3xxx_dev0.conf`, and `d15xx_dev0.conf`. Configuration changes require restarting the acceleration service.

## Runtime Configuration And Instances

- Intel's logical instance docs describe `[KERNEL]` and user-process sections separately. Kernel and user-space access are configured differently; avoid assuming a user-space QATzip/QATlib setup proves kernel API availability for ZFS.
- `NumberCyInstances` and `NumberDcInstances` control cryptographic and data-compression instances in the Intel config. Verify the active `[KERNEL]` or kernel-facing section; a loaded driver does not necessarily mean usable kernel compression or crypto instances exist.
- Intel documents service instances as API handles backed by queue pairs. This repository currently caps preallocated compression and crypto handles at `48`, so changes around instance sizing should compare repo caps with configured and reported QAT instances.
- Intel's docs note that over-large process/instance settings can preallocate resources and prevent driver load. Treat "more instances" as a capacity and memory tradeoff, not a free performance improvement.

## Compression API Facts

- This repository currently initializes compression with `CPA_DC_DEFLATE`, `CPA_DC_HT_FULL_DYNAMIC`, `CPA_DC_DIR_COMBINED`, `CPA_DC_STATELESS`, `CPA_DC_ADLER32`, and `CPA_DC_L1`.
- Intel's compression guide maps `CPA_DC_L1` to the lowest exposed compression level. Higher levels exist in the API, but support varies by hardware generation and can be rejected by the API.
- For QAT 1.x work in this fork, `zfs_qat_cpa_dc_level` is the global compression-level control and is intentionally limited to `CPA_DC_L1` through `CPA_DC_L4`.
- The QAT 4.28 header documents `CPA_DC_HT_FULL_DYNAMIC` as generating data-derived dynamic Huffman headers that require two passes, and `CPA_DC_HT_STATIC` as fixed Huffman trees. For QAT 1.x work in this fork, `zfs_qat_cpa_dc_hufftype` is the global Huffman-type control and is intentionally limited to `dynamic` and `static`.
- Intel documents `CPA_DC_FLUSH_FINAL` as the final-request flush flag for stateless Deflate compression and decompression. This matches the current `qat_compress.c` call pattern.
- Intel documents QAT compression status in `CpaDcRqResults.status`; `CPA_DC_OVERFLOW` is not necessarily a fatal hardware error and may require a larger destination buffer.
- The QAT 4.28 header `quickassist/include/dc/cpa_dc.h` exposes `cpaDcDeflateCompressBound()`, and the deployed access-layer `Module.symvers` exports that symbol. The implementation describes it as a synchronous helper for estimating worst-case Deflate output size to reduce overflow likelihood; it does not guarantee overflow is impossible in every exception case.
- The QAT 4.28 headers expose Deflate/LZ4 block-size capability concepts including 64 KiB, 256 KiB, 1 MiB, and 4 MiB bitmasks. This is API capability evidence, not proof that this OpenZFS path safely benefits from records above 128 KiB on dh895x/C620 without host validation.
- Intel recommends Compress-and-Verify for compression integrity and does not support disabling it in current docs. Be cautious with patches that bypass verification or hide CnV/CnVnR outcomes.
- Intel notes that some compression failures should result in storing the block uncompressed or compressing with software. That aligns with preserving OpenZFS software fallback behavior.

## Crypto And Checksum API Facts

- Intel's crypto guide lists AES-GCM and SHA256 support across QAT 1.7x, 1.8, and 2.0 tables. Still verify at runtime because `cpaCySymInitSession()` can return `CPA_STATUS_UNSUPPORTED` for unsupported algorithms.
- This repository accelerates AES-GCM encryption/decryption and SHA256 checksums through `module/os/linux/zfs/qat_crypt.c`; SHA512-family checksums are intentionally not covered by the current QAT checksum path.
- The current checksum code comments note that OpenZFS SHA512/256 uses a different IV from standard SHA512 and QAT does not support that variant. Do not generalize "QAT supports SHA512" into "QAT supports every ZFS SHA512-derived checksum."

## Performance And Memory Guidance

- Intel's performance guide frames QAT tuning around throughput, latency, and offload cost. Keep this distinction when changing thresholds or queueing behavior.
- The hardware interface is request/response oriented. Intel recommends asynchronous operation for best performance, but this repository exposes synchronous-looking helper calls around QAT requests; account for waiting, polling, and callback costs.
- Intel recommends minimizing buffer-list entries and says a single buffer per list gives best throughput. This matters because ZFS buffers may span pages and this implementation builds QAT buffer lists per request.
- Intel documents 64-byte payload alignment as optimal, while unaligned payloads may still work with lower performance. Avoid treating alignment advice as a correctness requirement unless the specific API structure requires it.
- Intel documents NUMA locality and memory-channel population as performance factors. Do not encode universal performance thresholds from a single machine or forum report.
- Intel documents SVM for QAT 2.0 and DMA-able/pinned memory requirements when SVM is not enabled. SVM is out of scope for this QAT 1.x-focused project; review allocation/copy costs in the current physically contiguous allocation path before lowering offload thresholds.
- Phase 4 host data on dh895xcc/QAT 4.28 showed 4 KiB gzip offload produced QAT DC failures, so compression now starts at 8 KiB while crypto/checksum retain the shared 4 KiB minimum.
- A phase 4 per-instance workspace experiment reduced metadata/list allocations but regressed elapsed time because it serialized requests behind per-instance locks. Do not reintroduce that shape without a concurrency-aware design and before/after data.

## Community Links From `ReferenceLinks.txt`

- OpenZFS discussion 12723 and TrueNAS forum/gist material contain useful first-hand build notes, especially around `ICP_ROOT`, `--enable-kapi`, service startup, and appliance-image constraints.
- Treat claims about which QAT generations "work with ZFS", IOMMU/SR-IOV workarounds, and TrueNAS packaging as environment-specific until verified with Intel docs and this repository's configure/runtime behavior.
- ServeTheHome hardware-generation summaries are useful for orientation only. Use Intel product and driver documentation for support claims.
