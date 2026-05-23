# QAT Performance Progression - 2026-05-23

Purpose: keep the imported benchmark/log corpus and a compact human-review table showing how QAT gzip performance changed as implementation and tuning changes were added.

## Imported Data

Imported host-root data is under `.codex/skills/openzfs-qat/artifacts/host-root-import-20260523/`. The normalized manifest is `.codex/skills/openzfs-qat/artifacts/host-root-import-20260523/MANIFEST.csv` and records `496` files copied from `/root` top-level benchmark/build artifacts.

| Ext | Files | Bytes |
| --- | --- | --- |
| .csv | 331 | 3113583 |
| .log | 158 | 578966 |
| .txt | 7 | 111218 |

Scope note: recursive source-tree snapshots under `/root/openzfs-qat-src-stage`, `/root/zfs-2.4.99.pre-*`, `/root/QAT`, and `/root/qatzip` were not imported as benchmark data because they duplicate source trees or third-party package contents rather than run artifacts. Top-level `/root` CSV/TXT/MD/LOG benchmark and build artifacts were imported.

## Reading The Tables

- `QAT vs SW` is same-window elapsed comparison: positive means QAT mode was faster than the matching software gzip row, negative means slower.
- `Delta vs initial` compares the QAT-vs-SW margin against the first controlled Phase 4 row for the same record size. It is a margin delta in percentage points, not a direct elapsed-time comparison across different harness generations.
- `QAT share` matters. A faster row with lower QAT share may be a good hybrid-policy outcome, but it is not proof that individual QAT requests became faster.
- CPU columns are not identical across the oldest runs. Initial manual rows only have CPU active percent; current harness rows generally use active CPU seconds per GiB.

## Original Manual Runs

These are the earliest imported real-data rows. They are useful provenance for out-of-box host behavior, but they lack QAT byte-share counters and are not normalized to the later harness.

| File | Dataset | Compression | Elapsed s | MiB/s | Ratio | CPU active % |
| --- | --- | --- | --- | --- | --- | --- |
| zfs-qat-realdata-v2-20260510-125527.csv | cpu-lz4 | lz4 | 83.641 | 238.72 | 1.05x | 2.26 |
| zfs-qat-realdata-v2-20260510-125527.csv | qat-gzip-1 | gzip | 80.534 | 247.93 | 1.05x | 1.87 |
| zfs-qat-realdata-v2-20260510-130731.csv | cpu-lz4 | lz4 | 82.308 | 242.58 | 1.05x | 2.37 |
| zfs-qat-realdata-v2-20260510-130731.csv | qat-gzip-1 | gzip-1 | 95.422 | 209.25 | 1.05x | 1.76 |
| zfs-qat-realdata-v2-20260510-131747.csv | cpu-lz4 | lz4 | 73.454 | 271.83 | 1.05x | 2.99 |
| zfs-qat-realdata-v2-20260510-131747.csv | qat-gzip-1 | gzip-1 | 83.478 | 239.18 | 1.06x | 20.94 |
| zfs-qat-realdata-v2-20260510-133053.csv | cpu-lz4 | lz4 | 84.525 | 236.22 | 1.05x | 2.28 |
| zfs-qat-realdata-v2-20260510-133053.csv | qat-gzip-1 | gzip-1 | 91.990 | 217.05 | 1.05x | 1.77 |

## Controlled Progression

| Date | Milestone | Change | Record/jobs | QAT ms | SW ms | QAT vs SW | Delta vs initial | QAT share | CPU reduction | Result |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-05-12 | Initial controlled baseline | Out-of-box static QAT path; no phase 4 optimizations yet | 128K/1 | 296.0 | 244.0 | -21.3% | 0.0 pp | +100.0% | +74.4% | Initial static QAT 128K offload baseline. |
| 2026-05-12 | Initial controlled baseline | Out-of-box static QAT path; no phase 4 optimizations yet | 1M/1 | 269.0 | 243.0 | -10.7% | 0.0 pp | 0.0% | +1.4% | Initial 1M QAT-mode row fell back to software because comp_requests=0. |
| 2026-05-17 | Async cap policy | Async cap policy=fixed | 1M/4 | 858.6 | 940.1 | +8.7% | +19.4 pp | +33.6% | +14.9% | Policy changes improve elapsed mostly by balancing QAT share and fallback, not by making every QAT request faster. |
| 2026-05-17 | Async cap policy | Async cap policy=recordsize | 1M/4 | 901.1 | 977.4 | +7.8% | +18.5 pp | +32.3% | +17.2% | Policy changes improve elapsed mostly by balancing QAT share and fallback, not by making every QAT request faster. |
| 2026-05-18 | Single card policy baseline | Single card policy baseline | 1M/8 | 1631.9 | 1710.5 | +4.6% | +15.3 pp | +36.3% | +38.7% | Dual-card scale helped 1M at higher concurrency but did not solve 128K latency. |
| 2026-05-18 | Dual card scale retest | Dual card scale retest | 1M/8 | 1346.8 | 1455.8 | +7.5% | +18.2 pp | +48.2% | +30.8% | Dual-card scale helped 1M at higher concurrency but did not solve 128K latency. |
| 2026-05-19 | DKMS lockstep regression | QAT/ZFS DKMS lockstep packaging; default 128K profile | 128K/4 | 1202.2 | 1098.2 | -9.5% | +11.8 pp | +100.0% | +69.0% | Functional QAT regression check; 128K still slower but CPU much lower. |
| 2026-05-19 | 1M profile target | Profile max-buffer target allows 1M records to use QAT | 1M/1 | 552.5 | 563.9 | +2.0% | +12.7 pp | +100.3% | +75.6% | Pure QAT offload row; concurrency still penalized service/wait time. |
| 2026-05-19 | 1M profile target | Profile max-buffer target allows 1M records to use QAT | 1M/4 | 1038.6 | 914.4 | -13.6% | -2.9 pp | +100.3% | +79.9% | Pure QAT offload row; concurrency still penalized service/wait time. |
| 2026-05-19 | 1M profile target | Profile max-buffer target allows 1M records to use QAT | 1M/8 | 1472.7 | 1377.2 | -6.9% | +3.8 pp | +100.8% | +81.2% | Pure QAT offload row; concurrency still penalized service/wait time. |
| 2026-05-21 | Async matrix | Async QAT enabled, interrupt completion | 1M/4 | 839.0 | 935.2 | +10.3% | +21.0 pp | +50.4% | +37.8% | Hybrid result; faster elapsed includes software fallback when cap is reached. |
| 2026-05-21 | Async matrix | Async QAT enabled, interrupt completion | 1M/8 | 1289.0 | 1734.7 | +25.7% | +36.4 pp | +65.1% | +55.3% | Hybrid result; faster elapsed includes software fallback when cap is reached. |
| 2026-05-21 | Async matrix | Async QAT enabled, poll completion | 1M/4 | 862.2 | 991.9 | +13.1% | +23.8 pp | +55.6% | +38.4% | Hybrid result; faster elapsed includes software fallback when cap is reached. |
| 2026-05-21 | Async matrix | Async QAT enabled, poll completion | 1M/8 | 1281.1 | 1470.3 | +12.9% | +23.6 pp | +59.0% | +39.7% | Hybrid result; faster elapsed includes software fallback when cap is reached. |
| 2026-05-22 | Service split | QAT service split cydc | 1M/4 | 929.2 | 968.5 | +4.1% | +14.8 pp | +79.9% | +53.5% | Comparable current best window; cy=0 improved byte share at jobs=4. |
| 2026-05-22 | Service split | QAT service split cydc | 1M/8 | 1243.8 | 1483.3 | +16.1% | +26.8 pp | +75.8% | +56.1% | Comparable current best window; cy=0 improved byte share at jobs=4. |
| 2026-05-22 | Service split | QAT service split cydc_kernelcy0 | 1M/4 | 907.0 | 958.1 | +5.3% | +16.0 pp | +90.3% | +59.5% | Comparable current best window; cy=0 improved byte share at jobs=4. |
| 2026-05-22 | Service split | QAT service split cydc_kernelcy0 | 1M/8 | 1297.0 | 1436.2 | +9.7% | +20.4 pp | +76.8% | +49.2% | Comparable current best window; cy=0 improved byte share at jobs=4. |
| 2026-05-23 | Kernel QAT polling smoke | [KERNEL_QAT] polling enabled | 1M/4 | 880.8 | 941.1 | +6.4% | +17.1 pp | +76.1% | +50.0% | Narrow smoke looked good, but the expanded matrix later showed mixed results. |
| 2026-05-23 | Kernel QAT polling smoke | [KERNEL_QAT] polling enabled | 1M/8 | 1231.3 | 1462.1 | +15.8% | +26.5 pp | +76.1% | +49.5% | Narrow smoke looked good, but the expanded matrix later showed mixed results. |
| 2026-05-23 | Completion matrix | Lockstep interrupt completion | 1M/4 | 868.2 | 925.6 | +6.2% | +16.9 pp | +76.6% | +50.4% | Expanded lockstep matrix; polling helped jobs=12 but regressed jobs=8. |
| 2026-05-23 | Completion matrix | Lockstep interrupt completion | 1M/8 | 1290.9 | 1486.7 | +13.2% | +23.9 pp | +78.5% | +55.1% | Expanded lockstep matrix; polling helped jobs=12 but regressed jobs=8. |
| 2026-05-23 | Completion matrix | Lockstep interrupt completion | 1M/12 | 1591.7 | 1624.7 | +2.0% | +12.7 pp | +71.3% | +41.8% | Expanded lockstep matrix; polling helped jobs=12 but regressed jobs=8. |
| 2026-05-23 | Completion matrix | Lockstep poll completion | 1M/4 | 933.5 | 1086.9 | +14.1% | +24.8 pp | +88.2% | +58.4% | Expanded lockstep matrix; polling helped jobs=12 but regressed jobs=8. |
| 2026-05-23 | Completion matrix | Lockstep poll completion | 1M/8 | 1574.0 | 1413.2 | -11.4% | -0.7 pp | +82.1% | +34.7% | Expanded lockstep matrix; polling helped jobs=12 but regressed jobs=8. |
| 2026-05-23 | Completion matrix | Lockstep poll completion | 1M/12 | 1456.8 | 1849.0 | +21.2% | +31.9 pp | +65.7% | +53.7% | Expanded lockstep matrix; polling helped jobs=12 but regressed jobs=8. |
| 2026-05-23 | Polling quota tuning | Polling quota=1 | 1M/8 | 1232.9 | 1413.2 | +12.8% | +23.5 pp | +77.8% | +50.4% | Improved the focused polling sweep; still a tuning candidate, not a default. |
| 2026-05-23 | Polling quota tuning | Polling quota=1 | 1M/12 | 1470.8 | 1849.0 | +20.5% | +31.2 pp | +65.7% | +53.1% | Improved the focused polling sweep; still a tuning candidate, not a default. |
| 2026-05-23 | QAT param check off | ICP_PARAM_CHECK=n | 1M/4 | 950.5 | 906.0 | -4.9% | +5.8 pp | +84.3% | +54.5% | Regression versus param-check-on baseline; not kept. |
| 2026-05-23 | QAT param check off | ICP_PARAM_CHECK=n | 1M/8 | 1323.9 | 1481.8 | +10.7% | +21.4 pp | +74.9% | +48.3% | Regression versus param-check-on baseline; not kept. |

## Tuning Outcomes

| Change | Jobs | QAT ms | QAT vs SW | QAT share | Decision |
| --- | --- | --- | --- | --- | --- |
| Async, source coalescing on | 4 | 905.4 | +0.6% | +88.5% | Source coalescing reduced source buffers but did not justify default enablement. |
| Lockstep interrupt completion | 4 | 868.2 | +6.2% | +76.6% | Expanded lockstep matrix; polling helped jobs=12 but regressed jobs=8. |
| Lockstep interrupt completion | 8 | 1290.9 | +13.2% | +78.5% | Expanded lockstep matrix; polling helped jobs=12 but regressed jobs=8. |
| Lockstep interrupt completion | 12 | 1591.7 | +2.0% | +71.3% | Expanded lockstep matrix; polling helped jobs=12 but regressed jobs=8. |
| Lockstep poll completion | 4 | 933.5 | +14.1% | +88.2% | Expanded lockstep matrix; polling helped jobs=12 but regressed jobs=8. |
| Lockstep poll completion | 8 | 1574.0 | -11.4% | +82.1% | Expanded lockstep matrix; polling helped jobs=12 but regressed jobs=8. |
| Lockstep poll completion | 12 | 1456.8 | +21.2% | +65.7% | Expanded lockstep matrix; polling helped jobs=12 but regressed jobs=8. |
| Polling quota=1 | 8 | 1232.9 | +12.8% | +77.8% | Improved the focused polling sweep; still a tuning candidate, not a default. |
| Polling quota=1 | 12 | 1470.8 | +20.5% | +65.7% | Improved the focused polling sweep; still a tuning candidate, not a default. |
| ICP_PARAM_CHECK=n | 4 | 950.5 | -4.9% | +84.3% | Regression versus param-check-on baseline; not kept. |
| ICP_PARAM_CHECK=n | 8 | 1323.9 | +10.7% | +74.9% | Regression versus param-check-on baseline; not kept. |

## Summary

- The initial controlled 128K path was about `21.3%` slower than software gzip while using QAT for essentially all compressed bytes. Later profile and async policy work reduced or reversed that margin in selected rows, mostly by using record-size-aware caps and preserving software fallback when QAT resources are the wrong tool.
- The initial 1M QAT-mode row did not offload to QAT. Later max-buffer/profile work made 1M records QAT-eligible; pure 1M QAT reduced CPU materially but was often slower at concurrency. Async hybrid policy then produced the first consistent elapsed-time wins at larger records.
- The best elapsed wins remain hybrid-policy wins, not pure QAT-engine wins. That is why `QAT share`, CPU cost, and same-window software rows are included beside every elapsed result.
- Source coalescing and QAT `ICP_PARAM_CHECK=n` did not earn default status. Polling remains mixed: narrow smoke and quota tuning can help some 1M concurrent rows, but the expanded lockstep matrix does not justify making polling the default.

Machine-readable selected milestone data: `.codex/skills/openzfs-qat/artifacts/performance-progression-summary-20260523.csv`.
