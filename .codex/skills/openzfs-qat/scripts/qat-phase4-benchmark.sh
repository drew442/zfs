#!/usr/bin/env bash
set -euo pipefail

POOL="${POOL:-test-hdd-pool}"
BENCH_ROOT="${BENCH_ROOT:-$POOL/bench}"
SOURCE="${SOURCE:-/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif}"
SOURCE_LABEL="${SOURCE_LABEL:-$(basename "$SOURCE" | tr -cs '[:alnum:]_.-' '_')}"
RECORDS="${RECORDS:-8K 16K 32K 64K 128K 256K 1M}"
MODES="${MODES:-qat sw}"
ITERS="${ITERS:-3}"
JOBS="${JOBS:-1}"
OUT="${OUT:-/root/zfs-qat-phase4-$(date +%Y%m%d-%H%M%S).csv}"

require_cmd() {
	command -v "$1" >/dev/null 2>&1 || {
		echo "Missing required command: $1" >&2
		exit 1
	}
}

require_cmd awk
require_cmd cmp
require_cmd cut
require_cmd grep
require_cmd modinfo
require_cmd numfmt
require_cmd sort
require_cmd stat
require_cmd seq
require_cmd tr
require_cmd zfs
require_cmd zpool

if [[ ! -f "$SOURCE" ]]; then
	echo "Source file does not exist: $SOURCE" >&2
	exit 1
fi

if ! zfs list -H "$BENCH_ROOT" >/dev/null 2>&1; then
	echo "Benchmark root dataset does not exist: $BENCH_ROOT" >&2
	exit 1
fi

QAT_PARAM_DIR="/sys/module/zfs/parameters"
QAT_KSTAT="/proc/spl/kstat/zfs/qat"

if [[ ! -r "$QAT_KSTAT" ]]; then
	echo "QAT kstat is not readable: $QAT_KSTAT" >&2
	exit 1
fi

read_param() {
	local name="$1"

	if [[ -r "$QAT_PARAM_DIR/$name" ]]; then
		cat "$QAT_PARAM_DIR/$name"
	else
		printf "na"
	fi
}

statv() {
	local name="$1"

	awk -v n="$name" '$1 == n { print $3; found = 1 }
	    END { if (!found) print 0 }' "$QAT_KSTAT"
}

drop_caches() {
	sync
	echo 3 > /proc/sys/vm/drop_caches || true
}

read_cpu() {
	awk '/^cpu / {
		user_j=$2
		nice_j=$3
		sys_j=$4
		idle_j=$5
		iowait_j=$6
		irq_j=$7
		softirq_j=$8
		steal_j=$9
		total_j=user_j+nice_j+sys_j+idle_j+iowait_j+irq_j+softirq_j+steal_j
		print user_j, nice_j, sys_j, idle_j, iowait_j, irq_j, softirq_j, steal_j, total_j
	}' /proc/stat
}

cpu_delta_csv() {
	local before="$1"
	local after="$2"

	awk -v b="$before" -v a="$after" '
	BEGIN {
		split(b, B, " ")
		split(a, A, " ")

		du = A[1] - B[1]
		dn = A[2] - B[2]
		ds = A[3] - B[3]
		didle = A[4] - B[4]
		diowait = A[5] - B[5]
		dirq = A[6] - B[6]
		dsoft = A[7] - B[7]
		dtotal = A[9] - B[9]

		if (dtotal <= 0)
			dtotal = 1

		printf "%.2f,%.2f,%.2f,%.2f",
		    100 * (du + dn) / dtotal,
		    100 * (ds + dirq + dsoft) / dtotal,
		    100 * diowait / dtotal,
		    100 * didle / dtotal
	}'
}

set_mode() {
	local mode="$1"

	case "$mode" in
	qat)
		echo 0 > "$QAT_PARAM_DIR/zfs_qat_compress_disable"
		;;
	sw)
		echo 1 > "$QAT_PARAM_DIR/zfs_qat_compress_disable"
		;;
	*)
		echo "Unknown mode: $mode" >&2
		exit 1
		;;
	esac
}

percentiles_csv() {
	local file="$1"

	sort -n "$file" | awk '
	function ceil(x) {
		return (x == int(x) ? x : int(x) + 1)
	}
	{
		v[++n] = $1
		sum += $1
	}
	END {
		if (n == 0) {
			print ",,,,"
			exit
		}

		p50 = ceil(n * 50 / 100)
		p95 = ceil(n * 95 / 100)
		p99 = ceil(n * 99 / 100)
		if (p50 < 1) p50 = 1
		if (p95 < 1) p95 = 1
		if (p99 < 1) p99 = 1
		if (p50 > n) p50 = n
		if (p95 > n) p95 = n
		if (p99 > n) p99 = n

		printf "%.3f,%.3f,%.3f,%.3f,%.3f\n",
		    sum / n, v[p50], v[p95], v[p99], v[n]
	}'
}

cleanup_ds() {
	local ds="$1"

	zfs destroy -r "$ds" 2>/dev/null || true
}

run_one() {
	local mode="$1"
	local record="$2"
	local iter="$3"
	local ds="$BENCH_ROOT/qat-phase4-${mode}-${record}-${iter}-$$"
	local mountpoint
	local cpu_before
	local cpu_after
	local cpu_csv
	local start_ns
	local end_ns
	local elapsed_ms
	local mib_s
	local comp_before
	local comp_after
	local comp_in_before
	local comp_in_after
	local comp_out_before
	local comp_out_after
	local decomp_before
	local decomp_after
	local decomp_in_before
	local decomp_in_after
	local decomp_out_before
	local decomp_out_after
	local fails_before
	local fails_after
	local reuse_hits_before
	local reuse_hits_after
	local reuse_misses_before
	local reuse_misses_after
	local ratio
	local used
	local logicalused
	local sha_ok="yes"
	local total_bytes
	local pids=()

	cleanup_ds "$ds"
	zfs create -o compression=gzip-1 -o checksum=sha256 \
	    -o recordsize="$record" "$ds"
	mountpoint="$(zfs get -H -o value mountpoint "$ds")"
	total_bytes=$((SOURCE_BYTES * JOBS))

	drop_caches
	comp_before="$(statv comp_requests)"
	comp_in_before="$(statv comp_total_in_bytes)"
	comp_out_before="$(statv comp_total_out_bytes)"
	decomp_before="$(statv decomp_requests)"
	decomp_in_before="$(statv decomp_total_in_bytes)"
	decomp_out_before="$(statv decomp_total_out_bytes)"
	fails_before="$(statv dc_fails)"
	reuse_hits_before="$(statv dc_buffer_reuse_hits)"
	reuse_misses_before="$(statv dc_buffer_reuse_misses)"
	cpu_before="$(read_cpu)"
	start_ns="$(date +%s%N)"

	for job in $(seq 1 "$JOBS"); do
		cp "$SOURCE" "$mountpoint/data-${job}.bin" &
		pids+=("$!")
	done

	for pid in "${pids[@]}"; do
		wait "$pid"
	done

	sync
	zpool sync "$POOL" || true

	pids=()
	for job in $(seq 1 "$JOBS"); do
		cmp "$SOURCE" "$mountpoint/data-${job}.bin" >/dev/null 2>&1 &
		pids+=("$!")
	done

	for pid in "${pids[@]}"; do
		if ! wait "$pid"; then
			sha_ok="no"
		fi
	done

	end_ns="$(date +%s%N)"
	cpu_after="$(read_cpu)"
	comp_after="$(statv comp_requests)"
	comp_in_after="$(statv comp_total_in_bytes)"
	comp_out_after="$(statv comp_total_out_bytes)"
	decomp_after="$(statv decomp_requests)"
	decomp_in_after="$(statv decomp_total_in_bytes)"
	decomp_out_after="$(statv decomp_total_out_bytes)"
	fails_after="$(statv dc_fails)"
	reuse_hits_after="$(statv dc_buffer_reuse_hits)"
	reuse_misses_after="$(statv dc_buffer_reuse_misses)"

	elapsed_ms="$(awk -v s="$start_ns" -v e="$end_ns" \
	    'BEGIN { printf "%.3f", (e - s) / 1000000 }')"
	mib_s="$(awk -v bytes="$total_bytes" -v ms="$elapsed_ms" \
	    'BEGIN {
		    if (ms <= 0) ms = 0.001
		    printf "%.2f", bytes / 1048576 / (ms / 1000)
	    }')"
	cpu_csv="$(cpu_delta_csv "$cpu_before" "$cpu_after")"
	ratio="$(zfs get -H -o value compressratio "$ds")"
	used="$(zfs get -H -o value used "$ds")"
	logicalused="$(zfs get -H -o value logicalused "$ds")"

	printf "raw,%s,%s,%s,%s,%s,%s,%s,,,,,,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
	    "$mode" "$record" "$iter" "$JOBS" "$SOURCE_LABEL" "$total_bytes" \
	    "$elapsed_ms" "$mib_s" "$cpu_csv" "$ratio" "$used" "$logicalused" \
	    "$((comp_after - comp_before))" \
	    "$((comp_in_after - comp_in_before))" \
	    "$((comp_out_after - comp_out_before))" \
	    "$((decomp_after - decomp_before))" \
	    "$((decomp_in_after - decomp_in_before))" \
	    "$((decomp_out_after - decomp_out_before))" \
	    "$((fails_after - fails_before))" \
	    "$((reuse_hits_after - reuse_hits_before))" \
	    "$((reuse_misses_after - reuse_misses_before))" \
	    "$sha_ok" "$QAT_DC_LEVEL" "$QAT_DC_MAX_BUF_SIZE" \
	    "$QAT_DC_MAX_INSTANCES" "$ZFS_SRCVERSION" |
	    tee -a "$OUT"

	printf "%s\n" "$elapsed_ms" >> "$LATENCY_FILE"
	cleanup_ds "$ds"

	if [[ "$sha_ok" != "yes" ]]; then
		echo "cmp failed for $mode recordsize=$record iter=$iter" >&2
		exit 1
	fi
}

ORIG_COMPRESS_DISABLE="$(read_param zfs_qat_compress_disable)"
trap 'if [[ "$ORIG_COMPRESS_DISABLE" != "na" ]]; then echo "$ORIG_COMPRESS_DISABLE" > "$QAT_PARAM_DIR/zfs_qat_compress_disable" || true; fi' EXIT

SOURCE_BYTES="$(stat -c %s "$SOURCE")"
QAT_DC_LEVEL="$(read_param zfs_qat_cpa_dc_level)"
QAT_DC_MAX_BUF_SIZE="$(read_param zfs_qat_dc_max_buf_size)"
QAT_DC_MAX_INSTANCES="$(read_param zfs_qat_dc_max_instances)"
ZFS_SRCVERSION="$(modinfo zfs | awk '$1 == "srcversion:" { print $2 }')"

mkdir -p "$(dirname "$OUT")"
printf "row_type,mode,recordsize,iter,jobs,source_label,source_bytes,elapsed_ms,latency_avg_ms,latency_p50_ms,latency_p95_ms,latency_p99_ms,latency_max_ms,write_bw_mib_s,cpu_user_pct,cpu_system_pct,cpu_iowait_pct,cpu_idle_pct,compressratio,used,logicalused,comp_requests_delta,comp_in_delta,comp_out_delta,decomp_requests_delta,decomp_in_delta,decomp_out_delta,dc_fails_delta,dc_buffer_reuse_hits_delta,dc_buffer_reuse_misses_delta,sha_ok,zfs_qat_cpa_dc_level,zfs_qat_dc_max_buf_size,zfs_qat_dc_max_instances,zfs_srcversion\n" > "$OUT"

echo "Results: $OUT" >&2
echo "Source: $SOURCE ($SOURCE_BYTES bytes)" >&2
echo "Modes: $MODES" >&2
echo "Records: $RECORDS" >&2
echo "Iterations: $ITERS" >&2
echo "Jobs: $JOBS" >&2

for mode in $MODES; do
	for record in $RECORDS; do
		LATENCY_FILE="$(mktemp "/tmp/qat-phase4-${mode}-${record}.XXXXXX")"
		set_mode "$mode"
		for iter in $(seq 1 "$ITERS"); do
			run_one "$mode" "$record" "$iter"
		done

		latency_csv="$(percentiles_csv "$LATENCY_FILE")"
		rm -f "$LATENCY_FILE"
		IFS=, read -r latency_avg latency_p50 latency_p95 \
		    latency_p99 latency_max <<< "$latency_csv"
		summary_row=(summary "$mode" "$record" "" "$JOBS" "$SOURCE_LABEL"
		    "$((SOURCE_BYTES * JOBS))" "" "$latency_avg" "$latency_p50"
		    "$latency_p95" "$latency_p99" "$latency_max" "" "" "" ""
		    "" "" "" "" "" "" "" "" "" "" "" "" "" "" "$QAT_DC_LEVEL"
		    "$QAT_DC_MAX_BUF_SIZE" "$QAT_DC_MAX_INSTANCES" "$ZFS_SRCVERSION")
		(IFS=,; printf "%s\n" "${summary_row[*]}") | tee -a "$OUT"
	done
done
