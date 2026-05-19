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
VERIFY_MODE="${VERIFY_MODE:-same}"
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
QAT_CONF="${QAT_CONF:-/etc/dh895xcc_dev0.conf}"
QAT_DRIVER_TIMING="${QAT_DRIVER_TIMING:-/proc/qat_dc_timing}"

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

driver_timingv() {
	local name="$1"

	if [[ ! -r "$QAT_DRIVER_TIMING" ]]; then
		printf "0"
		return
	fi

	awk -v n="$name" '$1 == n { print $2; found = 1 }
	    END { if (!found) print 0 }' "$QAT_DRIVER_TIMING"
}

avg_ns() {
	local total="$1"
	local count="$2"

	awk -v total="$total" -v count="$count" \
	    'BEGIN {
		if (count > 0)
			print int(total / count)
		else
			print "na"
	    }'
}

qat_conf_value() {
	local name="$1"

	if [[ ! -r "$QAT_CONF" ]]; then
		printf "na"
		return
	fi

	awk -v n="$name" '
	    $0 ~ /^\[KERNEL_QAT\]/ { in_section = 1; next }
	    $0 ~ /^\[/ && in_section { in_section = 0 }
	    in_section && $1 == n {
		print $3
		found = 1
		exit
	    }
	    END { if (!found) print "na" }' "$QAT_CONF"
}

qat_conf_file_count() {
	local count=0
	local conf

	for conf in /etc/dh895xcc_dev*.conf; do
		[[ -r "$conf" ]] || continue
		count=$((count + 1))
	done

	printf "%s" "$count"
}

qat_conf_sum() {
	local name="$1"
	local conf
	local sum=0

	for conf in /etc/dh895xcc_dev*.conf; do
		[[ -r "$conf" ]] || continue
		sum=$((sum + $(awk -v n="$name" '
		    $0 ~ /^\[KERNEL_QAT\]/ { in_section = 1; next }
		    $0 ~ /^\[/ && in_section { in_section = 0 }
		    in_section && $1 == n {
			print $3
			found = 1
			exit
		    }
		    END { if (!found) print 0 }' "$conf")))
	done

	printf "%s" "$sum"
}

qat_dh895xcc_pci_count() {
	local device
	local count=0

	for device in /sys/bus/pci/devices/*; do
		[[ -r "$device/vendor" && -r "$device/device" ]] || continue
		if [[ "$(cat "$device/vendor")" == "0x8086" &&
		    "$(cat "$device/device")" == "0x0435" ]]; then
			count=$((count + 1))
		fi
	done

	printf "%s" "$count"
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

online_cpu_count() {
	if command -v getconf >/dev/null 2>&1; then
		getconf _NPROCESSORS_ONLN 2>/dev/null && return
	fi

	if command -v nproc >/dev/null 2>&1; then
		nproc && return
	fi

	awk '/^processor[[:space:]]*:/ { n++ }
	    END { print n > 0 ? n : 1 }' /proc/cpuinfo
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

derived_metrics_csv() {
	local total_bytes="$1"
	local elapsed_ms="$2"
	local cpu_csv="$3"
	local comp_in_delta="$4"
	local async_submits_delta="$5"
	local async_completions_delta="$6"
	local async_fallbacks_delta="$7"
	local async_cap_skips_delta="$8"
	local comp_setup_delta="$9"
	local comp_submit_delta="${10}"
	local comp_wait_delta="${11}"
	local comp_cleanup_delta="${12}"

	awk -v total_bytes="$total_bytes" \
	    -v elapsed_ms="$elapsed_ms" \
	    -v cpu_csv="$cpu_csv" \
	    -v cpu_count="$CPU_COUNT" \
	    -v comp_in_delta="$comp_in_delta" \
	    -v async_submits_delta="$async_submits_delta" \
	    -v async_completions_delta="$async_completions_delta" \
	    -v async_fallbacks_delta="$async_fallbacks_delta" \
	    -v async_cap_skips_delta="$async_cap_skips_delta" \
	    -v comp_setup_delta="$comp_setup_delta" \
	    -v comp_submit_delta="$comp_submit_delta" \
	    -v comp_wait_delta="$comp_wait_delta" \
	    -v comp_cleanup_delta="$comp_cleanup_delta" '
	function pct(n, d) {
		return d > 0 ? sprintf("%.2f", 100 * n / d) : "na"
	}
	function seconds_per_gib(seconds, bytes) {
		return bytes > 0 ? sprintf("%.6f", seconds / (bytes / 1073741824)) : "na"
	}
	function ns_per_mib(ns, bytes) {
		return bytes > 0 ? sprintf("%.0f", ns / (bytes / 1048576)) : "na"
	}
	BEGIN {
		split(cpu_csv, cpu, ",")
		cpu_active_pct = cpu[1] + cpu[2]
		elapsed_s = elapsed_ms / 1000
		cpu_active_s = elapsed_s * cpu_count * cpu_active_pct / 100
		cpu_system_s = elapsed_s * cpu_count * cpu[2] / 100
		qat_service_ns = comp_setup_delta + comp_submit_delta + comp_wait_delta + comp_cleanup_delta

		printf "%s,%.2f,%s,%s,%s,%s,%s,%s,%s,%s",
		    cpu_count,
		    cpu_active_pct,
		    seconds_per_gib(cpu_active_s, total_bytes),
		    seconds_per_gib(cpu_system_s, total_bytes),
		    pct(comp_in_delta, total_bytes),
		    pct(async_completions_delta, async_submits_delta),
		    pct(async_fallbacks_delta, async_submits_delta),
		    pct(async_cap_skips_delta, async_submits_delta),
		    ns_per_mib(qat_service_ns, comp_in_delta),
		    ns_per_mib(comp_wait_delta, comp_in_delta)
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

set_decompress_mode() {
	local mode="$1"
	local param="$QAT_PARAM_DIR/zfs_qat_decompress_disable"

	if [[ ! -w "$param" ]]; then
		return
	fi

	case "$mode" in
	profile)
		echo profile > "$param"
		;;
	qat)
		echo 0 > "$param"
		;;
	sw)
		echo 1 > "$param"
		;;
	*)
		echo "Unknown decompression mode: $mode" >&2
		exit 1
		;;
	esac
}

effective_verify_mode() {
	local mode="$1"

	if [[ "$VERIFY_MODE" == "same" ]]; then
		printf "%s" "$mode"
	else
		printf "%s" "$VERIFY_MODE"
	fi
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
	local bound_requests_before
	local bound_requests_after
	local bound_fails_before
	local bound_fails_after
	local bound_ns_before
	local bound_ns_after
	local bound_total_before
	local bound_total_after
	local dst_total_before
	local dst_total_after
	local scratch_bytes_before
	local scratch_bytes_after
	local scratch_saved_before
	local scratch_saved_after
	local overflows_before
	local overflows_after
	local incompressible_before
	local incompressible_after
	local src_buffers_before
	local src_buffers_after
	local dst_buffers_before
	local dst_buffers_after
	local add_buffers_before
	local add_buffers_after
	local dst_total_buffers_before
	local dst_total_buffers_after
	local src_buffers_max_after
	local dst_buffers_max_after
	local add_buffers_max_after
	local dst_total_buffers_max_after
	local coalesce_requests_before
	local coalesce_requests_after
	local coalesce_success_before
	local coalesce_success_after
	local coalesce_fails_before
	local coalesce_fails_after
	local coalesce_bytes_before
	local coalesce_bytes_after
	local coalesce_alloc_before
	local coalesce_alloc_after
	local coalesce_copy_before
	local coalesce_copy_after
	local coalesce_free_before
	local coalesce_free_after
	local dst_coalesce_requests_before
	local dst_coalesce_requests_after
	local dst_coalesce_success_before
	local dst_coalesce_success_after
	local dst_coalesce_fails_before
	local dst_coalesce_fails_after
	local dst_coalesce_reuse_hits_before
	local dst_coalesce_reuse_hits_after
	local dst_coalesce_reuse_misses_before
	local dst_coalesce_reuse_misses_after
	local dst_coalesce_alloc_bytes_before
	local dst_coalesce_alloc_bytes_after
	local dst_coalesce_copy_bytes_before
	local dst_coalesce_copy_bytes_after
	local dst_coalesce_alloc_before
	local dst_coalesce_alloc_after
	local dst_coalesce_copy_before
	local dst_coalesce_copy_after
	local dst_coalesce_free_before
	local dst_coalesce_free_after
	local comp_scratch_alloc_before
	local comp_scratch_alloc_after
	local comp_scratch_free_before
	local comp_scratch_free_after
	local comp_setup_before
	local comp_setup_after
	local comp_submit_before
	local comp_submit_after
	local comp_wait_before
	local comp_wait_after
	local comp_cleanup_before
	local comp_cleanup_after
	local decomp_setup_before
	local decomp_setup_after
	local decomp_submit_before
	local decomp_submit_after
	local decomp_wait_before
	local decomp_wait_after
	local decomp_cleanup_before
	local decomp_cleanup_after
	local comp_inflight_after
	local comp_inflight_max_after
	local decomp_inflight_after
	local decomp_inflight_max_after
	local ratio
	local used
	local logicalused
	local sha_ok="yes"
	local total_bytes
	local verify_mode
	local decompress_disable
	local pids=()
	local comp_in_delta
	local comp_setup_delta
	local comp_submit_delta
	local comp_wait_delta
	local comp_cleanup_delta
	local async_submits_delta
	local async_completions_delta
	local async_fallbacks_delta
	local async_cap_skips_delta
	local driver_submits_before
	local driver_submits_after
	local driver_comp_submits_before
	local driver_comp_submits_after
	local driver_decomp_submits_before
	local driver_decomp_submits_after
	local driver_tx_retries_before
	local driver_tx_retries_after
	local driver_tx_errors_before
	local driver_tx_errors_after
	local driver_callbacks_before
	local driver_callbacks_after
	local driver_comp_callbacks_before
	local driver_comp_callbacks_after
	local driver_decomp_callbacks_before
	local driver_decomp_callbacks_after
	local driver_create_before
	local driver_create_after
	local driver_trans_put_before
	local driver_trans_put_after
	local driver_response_wait_before
	local driver_response_wait_after
	local driver_callback_process_before
	local driver_callback_process_after
	local driver_user_callback_before
	local driver_user_callback_after
	local driver_total_before
	local driver_total_after
	local driver_callbacks_delta
	local driver_response_wait_delta
	local driver_total_delta
	local derived_csv

	cleanup_ds "$ds"
	zfs create -o compression=gzip-1 -o checksum=sha256 \
	    -o recordsize="$record" "$ds"
	mountpoint="$(zfs get -H -o value mountpoint "$ds")"
	total_bytes=$((SOURCE_BYTES * JOBS))

	drop_caches
	set_decompress_mode "$mode"
	comp_before="$(statv comp_requests)"
	comp_in_before="$(statv comp_total_in_bytes)"
	comp_out_before="$(statv comp_total_out_bytes)"
	decomp_before="$(statv decomp_requests)"
	decomp_in_before="$(statv decomp_total_in_bytes)"
	decomp_out_before="$(statv decomp_total_out_bytes)"
	fails_before="$(statv dc_fails)"
	reuse_hits_before="$(statv dc_buffer_reuse_hits)"
	reuse_misses_before="$(statv dc_buffer_reuse_misses)"
	bound_requests_before="$(statv dc_compress_bound_requests)"
	bound_fails_before="$(statv dc_compress_bound_fails)"
	bound_ns_before="$(statv dc_compress_bound_ns)"
	bound_total_before="$(statv dc_compress_bound_total_bytes)"
	dst_total_before="$(statv dc_compress_dst_total_bytes)"
	scratch_bytes_before="$(statv dc_compress_scratch_bytes)"
	scratch_saved_before="$(statv dc_compress_scratch_saved_bytes)"
	overflows_before="$(statv dc_compress_overflows)"
	incompressible_before="$(statv dc_compress_incompressible)"
	src_buffers_before="$(statv dc_compress_src_buffers)"
	dst_buffers_before="$(statv dc_compress_dst_buffers)"
	add_buffers_before="$(statv dc_compress_add_buffers)"
	dst_total_buffers_before="$(statv dc_compress_dst_total_buffers)"
	coalesce_requests_before="$(statv dc_compress_coalesce_requests)"
	coalesce_success_before="$(statv dc_compress_coalesce_success)"
	coalesce_fails_before="$(statv dc_compress_coalesce_fails)"
	coalesce_bytes_before="$(statv dc_compress_coalesce_bytes)"
	coalesce_alloc_before="$(statv dc_compress_coalesce_alloc_ns)"
	coalesce_copy_before="$(statv dc_compress_coalesce_copy_ns)"
	coalesce_free_before="$(statv dc_compress_coalesce_free_ns)"
	dst_coalesce_requests_before="$(statv dc_compress_dst_coalesce_requests)"
	dst_coalesce_success_before="$(statv dc_compress_dst_coalesce_success)"
	dst_coalesce_fails_before="$(statv dc_compress_dst_coalesce_fails)"
	dst_coalesce_reuse_hits_before="$(statv dc_compress_dst_coalesce_reuse_hits)"
	dst_coalesce_reuse_misses_before="$(statv dc_compress_dst_coalesce_reuse_misses)"
	dst_coalesce_alloc_bytes_before="$(statv dc_compress_dst_coalesce_alloc_bytes)"
	dst_coalesce_copy_bytes_before="$(statv dc_compress_dst_coalesce_copy_bytes)"
	dst_coalesce_alloc_before="$(statv dc_compress_dst_coalesce_alloc_ns)"
	dst_coalesce_copy_before="$(statv dc_compress_dst_coalesce_copy_ns)"
	dst_coalesce_free_before="$(statv dc_compress_dst_coalesce_free_ns)"
	comp_scratch_alloc_before="$(statv dc_compress_scratch_alloc_ns)"
	comp_scratch_free_before="$(statv dc_compress_scratch_free_ns)"
	comp_setup_before="$(statv dc_compress_setup_ns)"
	comp_submit_before="$(statv dc_compress_submit_ns)"
	comp_wait_before="$(statv dc_compress_wait_ns)"
	comp_cleanup_before="$(statv dc_compress_cleanup_ns)"
	decomp_setup_before="$(statv dc_decompress_setup_ns)"
	decomp_submit_before="$(statv dc_decompress_submit_ns)"
	decomp_wait_before="$(statv dc_decompress_wait_ns)"
	decomp_cleanup_before="$(statv dc_decompress_cleanup_ns)"
	async_submits_before="$(statv dc_compress_async_submits)"
	async_submit_fails_before="$(statv dc_compress_async_submit_fails)"
	async_completions_before="$(statv dc_compress_async_completions)"
	async_resumes_before="$(statv dc_compress_async_resumes)"
	async_fallbacks_before="$(statv dc_compress_async_fallbacks)"
	async_cancels_before="$(statv dc_compress_async_cancels)"
	async_retries_before="$(statv dc_compress_async_submit_retries)"
	async_retry_success_before="$(statv dc_compress_async_retry_success)"
	async_fail_retry_before="$(statv dc_compress_async_fail_retry)"
	async_fail_resource_before="$(statv dc_compress_async_fail_resource)"
	async_fail_other_before="$(statv dc_compress_async_fail_other)"
	async_cap_skips_before="$(statv dc_compress_async_cap_skips)"
	driver_submits_before="$(driver_timingv submits)"
	driver_comp_submits_before="$(driver_timingv comp_submits)"
	driver_decomp_submits_before="$(driver_timingv decomp_submits)"
	driver_tx_retries_before="$(driver_timingv tx_retries)"
	driver_tx_errors_before="$(driver_timingv tx_errors)"
	driver_callbacks_before="$(driver_timingv callbacks)"
	driver_comp_callbacks_before="$(driver_timingv comp_callbacks)"
	driver_decomp_callbacks_before="$(driver_timingv decomp_callbacks)"
	driver_create_before="$(driver_timingv create_ns)"
	driver_trans_put_before="$(driver_timingv trans_put_ns)"
	driver_response_wait_before="$(driver_timingv response_wait_ns)"
	driver_callback_process_before="$(driver_timingv callback_process_ns)"
	driver_user_callback_before="$(driver_timingv user_callback_ns)"
	driver_total_before="$(driver_timingv total_ns)"
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

	verify_mode="$(effective_verify_mode "$mode")"
	set_decompress_mode "$verify_mode"
	decompress_disable="$(read_param zfs_qat_decompress_disable)"
	if [[ "$decompress_disable" == "profile" ]]; then
		if [[ "$QAT_DC_PROFILE" == "latency" ||
		    "$QAT_DC_PROFILE" == "throughput" ]]; then
			decompress_disable_effective="1"
		else
			decompress_disable_effective="0"
		fi
	else
		decompress_disable_effective="$decompress_disable"
	fi

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
	bound_requests_after="$(statv dc_compress_bound_requests)"
	bound_fails_after="$(statv dc_compress_bound_fails)"
	bound_ns_after="$(statv dc_compress_bound_ns)"
	bound_total_after="$(statv dc_compress_bound_total_bytes)"
	dst_total_after="$(statv dc_compress_dst_total_bytes)"
	scratch_bytes_after="$(statv dc_compress_scratch_bytes)"
	scratch_saved_after="$(statv dc_compress_scratch_saved_bytes)"
	overflows_after="$(statv dc_compress_overflows)"
	incompressible_after="$(statv dc_compress_incompressible)"
	src_buffers_after="$(statv dc_compress_src_buffers)"
	dst_buffers_after="$(statv dc_compress_dst_buffers)"
	add_buffers_after="$(statv dc_compress_add_buffers)"
	dst_total_buffers_after="$(statv dc_compress_dst_total_buffers)"
	src_buffers_max_after="$(statv dc_compress_src_buffers_max)"
	dst_buffers_max_after="$(statv dc_compress_dst_buffers_max)"
	add_buffers_max_after="$(statv dc_compress_add_buffers_max)"
	dst_total_buffers_max_after="$(statv dc_compress_dst_total_buffers_max)"
	coalesce_requests_after="$(statv dc_compress_coalesce_requests)"
	coalesce_success_after="$(statv dc_compress_coalesce_success)"
	coalesce_fails_after="$(statv dc_compress_coalesce_fails)"
	coalesce_bytes_after="$(statv dc_compress_coalesce_bytes)"
	coalesce_alloc_after="$(statv dc_compress_coalesce_alloc_ns)"
	coalesce_copy_after="$(statv dc_compress_coalesce_copy_ns)"
	coalesce_free_after="$(statv dc_compress_coalesce_free_ns)"
	dst_coalesce_requests_after="$(statv dc_compress_dst_coalesce_requests)"
	dst_coalesce_success_after="$(statv dc_compress_dst_coalesce_success)"
	dst_coalesce_fails_after="$(statv dc_compress_dst_coalesce_fails)"
	dst_coalesce_reuse_hits_after="$(statv dc_compress_dst_coalesce_reuse_hits)"
	dst_coalesce_reuse_misses_after="$(statv dc_compress_dst_coalesce_reuse_misses)"
	dst_coalesce_alloc_bytes_after="$(statv dc_compress_dst_coalesce_alloc_bytes)"
	dst_coalesce_copy_bytes_after="$(statv dc_compress_dst_coalesce_copy_bytes)"
	dst_coalesce_alloc_after="$(statv dc_compress_dst_coalesce_alloc_ns)"
	dst_coalesce_copy_after="$(statv dc_compress_dst_coalesce_copy_ns)"
	dst_coalesce_free_after="$(statv dc_compress_dst_coalesce_free_ns)"
	comp_scratch_alloc_after="$(statv dc_compress_scratch_alloc_ns)"
	comp_scratch_free_after="$(statv dc_compress_scratch_free_ns)"
	comp_setup_after="$(statv dc_compress_setup_ns)"
	comp_submit_after="$(statv dc_compress_submit_ns)"
	comp_wait_after="$(statv dc_compress_wait_ns)"
	comp_cleanup_after="$(statv dc_compress_cleanup_ns)"
	decomp_setup_after="$(statv dc_decompress_setup_ns)"
	decomp_submit_after="$(statv dc_decompress_submit_ns)"
	decomp_wait_after="$(statv dc_decompress_wait_ns)"
	decomp_cleanup_after="$(statv dc_decompress_cleanup_ns)"
	comp_inflight_after="$(statv dc_compress_inflight)"
	comp_inflight_max_after="$(statv dc_compress_inflight_max)"
	decomp_inflight_after="$(statv dc_decompress_inflight)"
	decomp_inflight_max_after="$(statv dc_decompress_inflight_max)"
	async_submits_after="$(statv dc_compress_async_submits)"
	async_submit_fails_after="$(statv dc_compress_async_submit_fails)"
	async_completions_after="$(statv dc_compress_async_completions)"
	async_resumes_after="$(statv dc_compress_async_resumes)"
	async_fallbacks_after="$(statv dc_compress_async_fallbacks)"
	async_cancels_after="$(statv dc_compress_async_cancels)"
	async_retries_after="$(statv dc_compress_async_submit_retries)"
	async_retry_success_after="$(statv dc_compress_async_retry_success)"
	async_fail_retry_after="$(statv dc_compress_async_fail_retry)"
	async_fail_resource_after="$(statv dc_compress_async_fail_resource)"
	async_fail_other_after="$(statv dc_compress_async_fail_other)"
	async_inflight_after="$(statv dc_compress_async_inflight)"
	async_inflight_max_after="$(statv dc_compress_async_inflight_max)"
	async_cap_skips_after="$(statv dc_compress_async_cap_skips)"
	driver_submits_after="$(driver_timingv submits)"
	driver_comp_submits_after="$(driver_timingv comp_submits)"
	driver_decomp_submits_after="$(driver_timingv decomp_submits)"
	driver_tx_retries_after="$(driver_timingv tx_retries)"
	driver_tx_errors_after="$(driver_timingv tx_errors)"
	driver_callbacks_after="$(driver_timingv callbacks)"
	driver_comp_callbacks_after="$(driver_timingv comp_callbacks)"
	driver_decomp_callbacks_after="$(driver_timingv decomp_callbacks)"
	driver_create_after="$(driver_timingv create_ns)"
	driver_trans_put_after="$(driver_timingv trans_put_ns)"
	driver_response_wait_after="$(driver_timingv response_wait_ns)"
	driver_callback_process_after="$(driver_timingv callback_process_ns)"
	driver_user_callback_after="$(driver_timingv user_callback_ns)"
	driver_total_after="$(driver_timingv total_ns)"

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
	comp_in_delta="$((comp_in_after - comp_in_before))"
	comp_setup_delta="$((comp_setup_after - comp_setup_before))"
	comp_submit_delta="$((comp_submit_after - comp_submit_before))"
	comp_wait_delta="$((comp_wait_after - comp_wait_before))"
	comp_cleanup_delta="$((comp_cleanup_after - comp_cleanup_before))"
	async_submits_delta="$((async_submits_after - async_submits_before))"
	async_completions_delta="$((async_completions_after - async_completions_before))"
	async_fallbacks_delta="$((async_fallbacks_after - async_fallbacks_before))"
	async_cap_skips_delta="$((async_cap_skips_after - async_cap_skips_before))"
	driver_callbacks_delta="$((driver_callbacks_after - driver_callbacks_before))"
	driver_response_wait_delta="$((driver_response_wait_after - driver_response_wait_before))"
	driver_total_delta="$((driver_total_after - driver_total_before))"
	derived_csv="$(derived_metrics_csv "$total_bytes" "$elapsed_ms" \
	    "$cpu_csv" "$comp_in_delta" "$async_submits_delta" \
	    "$async_completions_delta" "$async_fallbacks_delta" \
	    "$async_cap_skips_delta" "$comp_setup_delta" "$comp_submit_delta" \
	    "$comp_wait_delta" "$comp_cleanup_delta")"

	raw_row=(raw "$mode" "$verify_mode" "$record" "$iter" "$JOBS"
	    "$SOURCE_LABEL" "$total_bytes" "$elapsed_ms" "" "" "" "" ""
	    "$mib_s" "$cpu_csv" "$ratio" "$used" "$logicalused"
	    "$((comp_after - comp_before))"
	    "$comp_in_delta"
	    "$((comp_out_after - comp_out_before))"
	    "$((decomp_after - decomp_before))"
	    "$((decomp_in_after - decomp_in_before))"
	    "$((decomp_out_after - decomp_out_before))"
	    "$((fails_after - fails_before))"
	    "$((reuse_hits_after - reuse_hits_before))"
	    "$((reuse_misses_after - reuse_misses_before))"
	    "$((bound_requests_after - bound_requests_before))"
	    "$((bound_fails_after - bound_fails_before))"
	    "$((bound_ns_after - bound_ns_before))"
	    "$((bound_total_after - bound_total_before))"
	    "$((dst_total_after - dst_total_before))"
	    "$((scratch_bytes_after - scratch_bytes_before))"
	    "$((scratch_saved_after - scratch_saved_before))"
	    "$((overflows_after - overflows_before))"
	    "$((incompressible_after - incompressible_before))"
	    "$((src_buffers_after - src_buffers_before))"
	    "$((dst_buffers_after - dst_buffers_before))"
	    "$((add_buffers_after - add_buffers_before))"
	    "$((dst_total_buffers_after - dst_total_buffers_before))"
	    "$src_buffers_max_after" "$dst_buffers_max_after"
	    "$add_buffers_max_after" "$dst_total_buffers_max_after"
	    "$((coalesce_requests_after - coalesce_requests_before))"
	    "$((coalesce_success_after - coalesce_success_before))"
	    "$((coalesce_fails_after - coalesce_fails_before))"
	    "$((coalesce_bytes_after - coalesce_bytes_before))"
	    "$((coalesce_alloc_after - coalesce_alloc_before))"
	    "$((coalesce_copy_after - coalesce_copy_before))"
	    "$((coalesce_free_after - coalesce_free_before))"
	    "$((dst_coalesce_requests_after - dst_coalesce_requests_before))"
	    "$((dst_coalesce_success_after - dst_coalesce_success_before))"
	    "$((dst_coalesce_fails_after - dst_coalesce_fails_before))"
	    "$((dst_coalesce_reuse_hits_after - dst_coalesce_reuse_hits_before))"
	    "$((dst_coalesce_reuse_misses_after - dst_coalesce_reuse_misses_before))"
	    "$((dst_coalesce_alloc_bytes_after - dst_coalesce_alloc_bytes_before))"
	    "$((dst_coalesce_copy_bytes_after - dst_coalesce_copy_bytes_before))"
	    "$((dst_coalesce_alloc_after - dst_coalesce_alloc_before))"
	    "$((dst_coalesce_copy_after - dst_coalesce_copy_before))"
	    "$((dst_coalesce_free_after - dst_coalesce_free_before))"
	    "$((comp_scratch_alloc_after - comp_scratch_alloc_before))"
	    "$((comp_scratch_free_after - comp_scratch_free_before))"
	    "$comp_setup_delta"
	    "$comp_submit_delta"
	    "$comp_wait_delta"
	    "$comp_cleanup_delta"
	    "$((decomp_setup_after - decomp_setup_before))"
	    "$((decomp_submit_after - decomp_submit_before))"
	    "$((decomp_wait_after - decomp_wait_before))"
	    "$((decomp_cleanup_after - decomp_cleanup_before))"
	    "$comp_inflight_after" "$comp_inflight_max_after"
	    "$decomp_inflight_after" "$decomp_inflight_max_after"
	    "$async_submits_delta"
	    "$((async_submit_fails_after - async_submit_fails_before))"
	    "$async_completions_delta"
	    "$((async_resumes_after - async_resumes_before))"
	    "$async_fallbacks_delta"
	    "$((async_cancels_after - async_cancels_before))"
	    "$((async_retries_after - async_retries_before))"
	    "$((async_retry_success_after - async_retry_success_before))"
	    "$((async_fail_retry_after - async_fail_retry_before))"
	    "$((async_fail_resource_after - async_fail_resource_before))"
	    "$((async_fail_other_after - async_fail_other_before))"
	    "$async_inflight_after" "$async_inflight_max_after"
	    "$async_cap_skips_delta"
	    "$((driver_submits_after - driver_submits_before))"
	    "$((driver_comp_submits_after - driver_comp_submits_before))"
	    "$((driver_decomp_submits_after - driver_decomp_submits_before))"
	    "$((driver_tx_retries_after - driver_tx_retries_before))"
	    "$((driver_tx_errors_after - driver_tx_errors_before))"
	    "$driver_callbacks_delta"
	    "$((driver_comp_callbacks_after - driver_comp_callbacks_before))"
	    "$((driver_decomp_callbacks_after - driver_decomp_callbacks_before))"
	    "$((driver_create_after - driver_create_before))"
	    "$((driver_trans_put_after - driver_trans_put_before))"
	    "$driver_response_wait_delta"
	    "$((driver_callback_process_after - driver_callback_process_before))"
	    "$((driver_user_callback_after - driver_user_callback_before))"
	    "$driver_total_delta"
	    "$(avg_ns "$driver_response_wait_delta" "$driver_callbacks_delta")"
	    "$(avg_ns "$driver_total_delta" "$driver_callbacks_delta")"
	    "$sha_ok" "$QAT_DC_LEVEL" "$QAT_DC_EFFECTIVE_LEVEL"
	    "$QAT_DC_HUFFTYPE" "$QAT_DC_EFFECTIVE_HUFFTYPE"
	    "$QAT_DC_MAX_BUF_SIZE" "$QAT_DC_EFFECTIVE_MAX_BUF_SIZE"
	    "$QAT_DC_MAX_INSTANCES"
	    "$QAT_DC_COALESCE_SRC" "$QAT_DC_EFFECTIVE_COALESCE_SRC"
	    "$QAT_DC_COALESCE_DST" "$QAT_DC_EFFECTIVE_COALESCE_DST"
	    "$QAT_DC_ASYNC" "$QAT_DC_EFFECTIVE_ASYNC"
	    "$QAT_DC_ASYNC_RETRIES" "$QAT_DC_EFFECTIVE_ASYNC_RETRIES"
	    "$QAT_DC_ASYNC_RETRY_US" "$QAT_DC_EFFECTIVE_ASYNC_RETRY_US"
	    "$QAT_DC_ASYNC_MAX_INFLIGHT"
	    "$QAT_DC_EFFECTIVE_ASYNC_MAX_INFLIGHT" "$QAT_DC_ASYNC_CAP_POLICY"
	    "$QAT_DC_PROFILE" "$QAT_DC_PROFILE_RECORDSIZE"
	    "$QAT_DC_RATIO_PROFILE"
	    "$decompress_disable" "$decompress_disable_effective"
	    "$QAT_KERNEL_CY_INSTANCES"
	    "$QAT_KERNEL_DC_INSTANCES" "$ZFS_SRCVERSION"
	    "$QAT_PCI_DH895XCC_COUNT" "$QAT_CONF_FILE_COUNT"
	    "$QAT_KERNEL_CY_INSTANCES_TOTAL"
	    "$QAT_KERNEL_DC_INSTANCES_TOTAL"
	    "$(statv dc_instances)"
	    "$derived_csv")
	(IFS=,; printf "%s\n" "${raw_row[*]}") | tee -a "$OUT"

	printf "%s\n" "$elapsed_ms" >> "$LATENCY_FILE"
	cleanup_ds "$ds"

	if [[ "$sha_ok" != "yes" ]]; then
		echo "cmp failed for $mode recordsize=$record iter=$iter" >&2
		exit 1
	fi
}

ORIG_COMPRESS_DISABLE="$(read_param zfs_qat_compress_disable)"
ORIG_DECOMPRESS_DISABLE="$(read_param zfs_qat_decompress_disable)"
trap 'if [[ "$ORIG_COMPRESS_DISABLE" != "na" ]]; then echo "$ORIG_COMPRESS_DISABLE" > "$QAT_PARAM_DIR/zfs_qat_compress_disable" || true; fi; if [[ "$ORIG_DECOMPRESS_DISABLE" != "na" ]]; then echo "$ORIG_DECOMPRESS_DISABLE" > "$QAT_PARAM_DIR/zfs_qat_decompress_disable" || true; fi' EXIT

SOURCE_BYTES="$(stat -c %s "$SOURCE")"
QAT_DC_LEVEL="$(read_param zfs_qat_cpa_dc_level)"
QAT_DC_HUFFTYPE="$(read_param zfs_qat_cpa_dc_hufftype)"
QAT_DC_MAX_BUF_SIZE="$(read_param zfs_qat_dc_max_buf_size)"
QAT_DC_MAX_INSTANCES="$(read_param zfs_qat_dc_max_instances)"
QAT_DC_COALESCE_SRC="$(read_param zfs_qat_dc_coalesce_src)"
QAT_DC_COALESCE_DST="$(read_param zfs_qat_dc_coalesce_dst)"
QAT_DC_ASYNC="$(read_param zfs_qat_dc_async)"
QAT_DC_ASYNC_RETRIES="$(read_param zfs_qat_dc_async_submit_retries)"
QAT_DC_ASYNC_RETRY_US="$(read_param zfs_qat_dc_async_retry_us)"
QAT_DC_ASYNC_MAX_INFLIGHT="$(read_param zfs_qat_dc_async_max_inflight)"
QAT_DC_ASYNC_CAP_POLICY="$(read_param zfs_qat_dc_async_cap_policy)"
QAT_DC_PROFILE="$(read_param zfs_qat_dc_profile)"
QAT_DC_PROFILE_RECORDSIZE="$(read_param zfs_qat_dc_profile_recordsize)"
QAT_DC_RATIO_PROFILE="$(read_param zfs_qat_dc_ratio_profile)"
if [[ "$QAT_DC_LEVEL" == "profile" ]]; then
	if [[ "$QAT_DC_RATIO_PROFILE" == "ratio" ]]; then
		QAT_DC_EFFECTIVE_LEVEL="4"
	else
		QAT_DC_EFFECTIVE_LEVEL="1"
	fi
else
	QAT_DC_EFFECTIVE_LEVEL="$QAT_DC_LEVEL"
fi
if [[ "$QAT_DC_HUFFTYPE" == "profile" ]]; then
	if [[ "$QAT_DC_RATIO_PROFILE" == "performance" ]]; then
		QAT_DC_EFFECTIVE_HUFFTYPE="static"
	else
		QAT_DC_EFFECTIVE_HUFFTYPE="dynamic"
	fi
else
	QAT_DC_EFFECTIVE_HUFFTYPE="$QAT_DC_HUFFTYPE"
fi
if [[ "$QAT_DC_MAX_BUF_SIZE" == "profile" ]]; then
	QAT_DC_EFFECTIVE_MAX_BUF_SIZE="$QAT_DC_PROFILE_RECORDSIZE"
else
	QAT_DC_EFFECTIVE_MAX_BUF_SIZE="$QAT_DC_MAX_BUF_SIZE"
fi
if [[ "$QAT_DC_COALESCE_SRC" == "profile" ]]; then
	QAT_DC_EFFECTIVE_COALESCE_SRC="0"
else
	QAT_DC_EFFECTIVE_COALESCE_SRC="$QAT_DC_COALESCE_SRC"
fi
if [[ "$QAT_DC_COALESCE_DST" == "profile" ]]; then
	QAT_DC_EFFECTIVE_COALESCE_DST="0"
else
	QAT_DC_EFFECTIVE_COALESCE_DST="$QAT_DC_COALESCE_DST"
fi
if [[ "$QAT_DC_ASYNC" == "profile" ]]; then
	if [[ "$QAT_DC_PROFILE" == "throughput" ||
	    "$QAT_DC_PROFILE" == "offload" ]]; then
		QAT_DC_EFFECTIVE_ASYNC="1"
	else
		QAT_DC_EFFECTIVE_ASYNC="0"
	fi
else
	QAT_DC_EFFECTIVE_ASYNC="$QAT_DC_ASYNC"
fi
if [[ "$QAT_DC_ASYNC_RETRIES" == "profile" ]]; then
	QAT_DC_EFFECTIVE_ASYNC_RETRIES="8"
else
	QAT_DC_EFFECTIVE_ASYNC_RETRIES="$QAT_DC_ASYNC_RETRIES"
fi
if [[ "$QAT_DC_ASYNC_RETRY_US" == "profile" ]]; then
	QAT_DC_EFFECTIVE_ASYNC_RETRY_US="100"
else
	QAT_DC_EFFECTIVE_ASYNC_RETRY_US="$QAT_DC_ASYNC_RETRY_US"
fi
if [[ "$QAT_DC_ASYNC_MAX_INFLIGHT" == "profile" ]]; then
	QAT_DC_EFFECTIVE_ASYNC_MAX_INFLIGHT="96"
else
	QAT_DC_EFFECTIVE_ASYNC_MAX_INFLIGHT="$QAT_DC_ASYNC_MAX_INFLIGHT"
fi
QAT_KERNEL_CY_INSTANCES="$(qat_conf_value NumberCyInstances)"
QAT_KERNEL_DC_INSTANCES="$(qat_conf_value NumberDcInstances)"
QAT_PCI_DH895XCC_COUNT="$(qat_dh895xcc_pci_count)"
QAT_CONF_FILE_COUNT="$(qat_conf_file_count)"
QAT_KERNEL_CY_INSTANCES_TOTAL="$(qat_conf_sum NumberCyInstances)"
QAT_KERNEL_DC_INSTANCES_TOTAL="$(qat_conf_sum NumberDcInstances)"
ZFS_SRCVERSION="$(modinfo zfs | awk '$1 == "srcversion:" { print $2 }')"
CPU_COUNT="$(online_cpu_count)"

mkdir -p "$(dirname "$OUT")"
printf "row_type,mode,verify_mode,recordsize,iter,jobs,source_label,source_bytes,elapsed_ms,latency_avg_ms,latency_p50_ms,latency_p95_ms,latency_p99_ms,latency_max_ms,write_bw_mib_s,cpu_user_pct,cpu_system_pct,cpu_iowait_pct,cpu_idle_pct,compressratio,used,logicalused,comp_requests_delta,comp_in_delta,comp_out_delta,decomp_requests_delta,decomp_in_delta,decomp_out_delta,dc_fails_delta,dc_buffer_reuse_hits_delta,dc_buffer_reuse_misses_delta,dc_compress_bound_requests_delta,dc_compress_bound_fails_delta,dc_compress_bound_ns_delta,dc_compress_bound_total_bytes_delta,dc_compress_dst_total_bytes_delta,dc_compress_scratch_bytes_delta,dc_compress_scratch_saved_bytes_delta,dc_compress_overflows_delta,dc_compress_incompressible_delta,dc_compress_src_buffers_delta,dc_compress_dst_buffers_delta,dc_compress_add_buffers_delta,dc_compress_dst_total_buffers_delta,dc_compress_src_buffers_max,dc_compress_dst_buffers_max,dc_compress_add_buffers_max,dc_compress_dst_total_buffers_max,dc_compress_coalesce_requests_delta,dc_compress_coalesce_success_delta,dc_compress_coalesce_fails_delta,dc_compress_coalesce_bytes_delta,dc_compress_coalesce_alloc_ns_delta,dc_compress_coalesce_copy_ns_delta,dc_compress_coalesce_free_ns_delta,dc_compress_dst_coalesce_requests_delta,dc_compress_dst_coalesce_success_delta,dc_compress_dst_coalesce_fails_delta,dc_compress_dst_coalesce_reuse_hits_delta,dc_compress_dst_coalesce_reuse_misses_delta,dc_compress_dst_coalesce_alloc_bytes_delta,dc_compress_dst_coalesce_copy_bytes_delta,dc_compress_dst_coalesce_alloc_ns_delta,dc_compress_dst_coalesce_copy_ns_delta,dc_compress_dst_coalesce_free_ns_delta,dc_compress_scratch_alloc_ns_delta,dc_compress_scratch_free_ns_delta,dc_compress_setup_ns_delta,dc_compress_submit_ns_delta,dc_compress_wait_ns_delta,dc_compress_cleanup_ns_delta,dc_decompress_setup_ns_delta,dc_decompress_submit_ns_delta,dc_decompress_wait_ns_delta,dc_decompress_cleanup_ns_delta,dc_compress_inflight,dc_compress_inflight_max,dc_decompress_inflight,dc_decompress_inflight_max,dc_compress_async_submits_delta,dc_compress_async_submit_fails_delta,dc_compress_async_completions_delta,dc_compress_async_resumes_delta,dc_compress_async_fallbacks_delta,dc_compress_async_cancels_delta,dc_compress_async_submit_retries_delta,dc_compress_async_retry_success_delta,dc_compress_async_fail_retry_delta,dc_compress_async_fail_resource_delta,dc_compress_async_fail_other_delta,dc_compress_async_inflight,dc_compress_async_inflight_max,dc_compress_async_cap_skips_delta,qat_driver_submits_delta,qat_driver_comp_submits_delta,qat_driver_decomp_submits_delta,qat_driver_tx_retries_delta,qat_driver_tx_errors_delta,qat_driver_callbacks_delta,qat_driver_comp_callbacks_delta,qat_driver_decomp_callbacks_delta,qat_driver_create_ns_delta,qat_driver_trans_put_ns_delta,qat_driver_response_wait_ns_delta,qat_driver_callback_process_ns_delta,qat_driver_user_callback_ns_delta,qat_driver_total_ns_delta,qat_driver_avg_response_wait_ns,qat_driver_avg_total_ns,sha_ok,zfs_qat_cpa_dc_level,zfs_qat_effective_cpa_dc_level,zfs_qat_cpa_dc_hufftype,zfs_qat_effective_cpa_dc_hufftype,zfs_qat_dc_max_buf_size,zfs_qat_dc_effective_max_buf_size,zfs_qat_dc_max_instances,zfs_qat_dc_coalesce_src,zfs_qat_dc_effective_coalesce_src,zfs_qat_dc_coalesce_dst,zfs_qat_dc_effective_coalesce_dst,zfs_qat_dc_async,zfs_qat_dc_effective_async,zfs_qat_dc_async_submit_retries,zfs_qat_dc_effective_async_submit_retries,zfs_qat_dc_async_retry_us,zfs_qat_dc_effective_async_retry_us,zfs_qat_dc_async_max_inflight,zfs_qat_dc_effective_async_max_inflight,zfs_qat_dc_async_cap_policy,zfs_qat_dc_profile,zfs_qat_dc_profile_recordsize,zfs_qat_dc_ratio_profile,zfs_qat_decompress_disable,zfs_qat_effective_decompress_disable,qat_kernel_cy_instances,qat_kernel_dc_instances,zfs_srcversion,qat_pci_dh895xcc_count,qat_conf_file_count,qat_kernel_cy_instances_total,qat_kernel_dc_instances_total,zfs_qat_dc_instances,cpu_count,cpu_active_pct,cpu_active_s_per_gib,cpu_system_s_per_gib,qat_byte_share_pct,qat_completion_share_pct,qat_fallback_share_pct,qat_cap_skip_share_pct,qat_service_ns_per_mib,qat_wait_ns_per_mib\n" > "$OUT"

echo "Results: $OUT" >&2
echo "Source: $SOURCE ($SOURCE_BYTES bytes)" >&2
echo "Modes: $MODES" >&2
echo "Verify mode: $VERIFY_MODE" >&2
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
		verify_mode="$(effective_verify_mode "$mode")"
		summary_row=(summary "$mode" "$verify_mode" "$record" "" "$JOBS"
		    "$SOURCE_LABEL" "$((SOURCE_BYTES * JOBS))" "" "$latency_avg" "$latency_p50"
		    "$latency_p95" "$latency_p99" "$latency_max" "" "" "" ""
		    "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" ""
		    "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" ""
		    "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" ""
		    "" ""
		    "" ""
		    "" "" "" "" "" "" "" "" "" "" "" "" "" ""
		    "" "" "" "" "" "" "" ""
		    "" "" "" "" "" "" "" ""
		    "$QAT_DC_LEVEL" "$QAT_DC_EFFECTIVE_LEVEL"
		    "$QAT_DC_HUFFTYPE" "$QAT_DC_EFFECTIVE_HUFFTYPE"
		    "$QAT_DC_MAX_BUF_SIZE" "$QAT_DC_EFFECTIVE_MAX_BUF_SIZE"
		    "$QAT_DC_MAX_INSTANCES"
		    "$QAT_DC_COALESCE_SRC" "$QAT_DC_EFFECTIVE_COALESCE_SRC"
		    "$QAT_DC_COALESCE_DST" "$QAT_DC_EFFECTIVE_COALESCE_DST"
		    "$QAT_DC_ASYNC" "$QAT_DC_EFFECTIVE_ASYNC"
		    "$QAT_DC_ASYNC_RETRIES" "$QAT_DC_EFFECTIVE_ASYNC_RETRIES"
		    "$QAT_DC_ASYNC_RETRY_US" "$QAT_DC_EFFECTIVE_ASYNC_RETRY_US"
		    "$QAT_DC_ASYNC_MAX_INFLIGHT"
		    "$QAT_DC_EFFECTIVE_ASYNC_MAX_INFLIGHT"
		    "$QAT_DC_ASYNC_CAP_POLICY" "$QAT_DC_PROFILE"
		    "$QAT_DC_PROFILE_RECORDSIZE" "$QAT_DC_RATIO_PROFILE"
		    "" "" "$QAT_KERNEL_CY_INSTANCES"
		    "$QAT_KERNEL_DC_INSTANCES"
		    "$ZFS_SRCVERSION" "$QAT_PCI_DH895XCC_COUNT"
		    "$QAT_CONF_FILE_COUNT" "$QAT_KERNEL_CY_INSTANCES_TOTAL"
		    "$QAT_KERNEL_DC_INSTANCES_TOTAL"
		    "$(statv dc_instances)"
		    "" "" "" "" "" "" "" "" "" "")
		(IFS=,; printf "%s\n" "${summary_row[*]}") | tee -a "$OUT"
	done
done
