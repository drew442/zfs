# QAT Profile Sweep - 2026-05-18

Purpose: compare the profile-managed QAT policies after all planned profile
tunables were converted to `profile|manual` behavior.

Benchmark shape:

- Host: `pve.drewnet.online`
- ZFS module `srcversion`: `62EF6B1D2C6A51421EB3DD2`
- QAT cards: 2x DH895XCC
- Iterations: `3`
- Jobs: `4`
- Records: `128K 256K 1M`
- Modes: `qat sw`
- Verification: `VERIFY_MODE=profile`
- Target record sizes tested: `128K`, `256K`, `512K`, `1M`

Raw CSVs:

```text
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target128k-balanced-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target128k-latency-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target128k-throughput-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target128k-offload-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target256k-balanced-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target256k-latency-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target256k-throughput-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target256k-offload-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target512k-balanced-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target512k-latency-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target512k-throughput-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target512k-offload-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target1m-balanced-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target1m-latency-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target1m-throughput-20260518.csv
.codex/skills/openzfs-qat/references/benchmarks/zfs-qat-profile-sweep-target1m-offload-20260518.csv
```

## Findings

- The only clear QAT throughput win in this sweep was `target=1M`,
  `profile=throughput`, `record=1M`: QAT was `7.5%` faster than same-window
  software and used about half the system CPU seconds per GiB.
- `target=1M`, `profile=throughput`, `record=1M` was a hybrid result:
  `72.7%` QAT byte share and `27.5%` fallback share. The win is useful, but it
  is not pure hardware acceleration.
- Fully synchronous QAT offload reduced system CPU substantially, but it was
  usually slower than software on elapsed time.
- Apparent wins with `qat_byte=0.0` are software-fallback wins, not QAT
  performance wins.
- The `latency` profile reduced the QAT-readback penalty by using software
  readback, but it did not create a broad QAT-vs-software elapsed-time win.
- `offload` increased QAT use and reduced CPU versus software in several rows,
  but it did not beat software elapsed time in the 1M target sweep.

## Summary Table

`QAT vs SW` is elapsed-time delta. Negative is faster than software.

| Target | Profile | Record | QAT ms | SW ms | QAT vs SW | QAT sys CPU/GiB | SW sys CPU/GiB | QAT byte % | Fallback % | Async | SW read |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 128K | balanced | 128K | 1156.9 | 1047.8 | +10.4% | 3.77 | 12.90 | 100.0 | na | 0 | 0 |
| 128K | latency | 128K | 1104.8 | 1041.3 | +6.1% | 4.88 | 13.38 | 100.0 | na | 0 | 1 |
| 128K | throughput | 128K | 1178.9 | 1045.2 | +12.8% | 9.26 | 12.35 | 73.1 | 26.9 | 1 | 1 |
| 128K | offload | 128K | 1183.4 | 1056.9 | +12.0% | 8.00 | 12.42 | 80.6 | 19.4 | 1 | 0 |
| 256K | balanced | 256K | 1075.6 | 1054.7 | +2.0% | 3.02 | 12.05 | 100.0 | na | 0 | 0 |
| 256K | latency | 256K | 1055.4 | 956.7 | +10.3% | 4.24 | 11.28 | 100.0 | na | 0 | 1 |
| 256K | throughput | 256K | 1014.6 | 972.2 | +4.4% | 8.31 | 11.67 | 47.1 | 52.9 | 1 | 1 |
| 256K | offload | 256K | 1074.8 | 988.2 | +8.8% | 7.20 | 11.63 | 49.4 | 50.6 | 1 | 0 |
| 512K | balanced | 256K | 1042.4 | 1008.6 | +3.4% | 2.88 | 11.80 | 100.0 | na | 0 | 0 |
| 512K | latency | 256K | 1014.0 | 998.3 | +1.6% | 4.04 | 11.80 | 100.0 | na | 0 | 1 |
| 512K | throughput | 256K | 1092.5 | 990.0 | +10.3% | 8.11 | 11.21 | 47.2 | 52.8 | 1 | 1 |
| 512K | offload | 256K | 1093.8 | 1002.5 | +9.1% | 7.36 | 12.22 | 48.5 | 51.5 | 1 | 0 |
| 1M | balanced | 1M | 1009.1 | 927.8 | +8.8% | 2.13 | 11.18 | 100.3 | na | 0 | 0 |
| 1M | latency | 1M | 990.1 | 960.0 | +3.1% | 3.45 | 11.31 | 100.3 | na | 0 | 1 |
| 1M | throughput | 1M | 844.7 | 913.3 | -7.5% | 5.66 | 11.24 | 72.7 | 27.5 | 1 | 1 |
| 1M | offload | 1M | 950.3 | 920.5 | +3.2% | 3.23 | 11.22 | 92.0 | 8.3 | 1 | 0 |

## Interpretation

- Keep `throughput` for 1M as the only profile candidate that currently shows a
  clear elapsed-time win.
- Do not promote `balanced` large-record full offload as a latency-safe default
  without a policy change or repeat evidence; it saves CPU but regresses elapsed
  time.
- Treat `offload` as a CPU/QAT-share profile, not a throughput profile.
- Repeat the `target=1M`, `profile=throughput`, `record=1M` result with more
  iterations and at `JOBS=8` before changing the profile mappings again.
