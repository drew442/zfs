#!/usr/bin/env bash
set -euo pipefail

HARNESS="${HARNESS:-/usr/src/zfs-2.4.99/.codex/skills/openzfs-qat/scripts/qat-phase4-benchmark.sh}"
OUT_DIR="${OUT_DIR:-/root}"
ITERS="${ITERS:-3}"
RECORDS="${RECORDS:-128K 512K 1M}"
JOBS_LIST="${JOBS_LIST:-1 4 8}"
MODES="${MODES:-qat sw}"
VERIFY_MODE="${VERIFY_MODE:-sw}"
RUN_ORDER="${RUN_ORDER:-record}"

QAT_PARAM_DIR="/sys/module/zfs/parameters"

require_file() {
	local path="$1"

	if [[ ! -e "$path" ]]; then
		echo "Missing required path: $path" >&2
		exit 1
	fi
}

read_param() {
	local name="$1"

	cat "$QAT_PARAM_DIR/$name"
}

require_file "$HARNESS"
require_file "$QAT_PARAM_DIR/zfs_qat_dc_expected_ratio"
require_file /nvme_scratch/bench
require_file /test-hdd-pool/bench

expected_ratio="$(read_param zfs_qat_dc_expected_ratio)"
profile="$(read_param zfs_qat_dc_profile)"
ratio_profile="$(read_param zfs_qat_dc_ratio_profile)"
profile_recordsize="$(read_param zfs_qat_dc_profile_recordsize)"

if [[ "$profile" != "balanced" ||
    "$ratio_profile" != "balanced" ||
    "$profile_recordsize" != "1048576" ]]; then
	echo "Unexpected active profile state:" >&2
	echo "  zfs_qat_dc_profile=$profile" >&2
	echo "  zfs_qat_dc_ratio_profile=$ratio_profile" >&2
	echo "  zfs_qat_dc_profile_recordsize=$profile_recordsize" >&2
	echo "Expected balanced/balanced/1048576 for this matrix." >&2
	exit 1
fi

declare -a MEDIA_NAMES=("nvme" "hdd")
declare -a MEDIA_ROOTS=("nvme_scratch/bench" "test-hdd-pool/bench")
declare -a SOURCE_NAMES=("random" "mixed" "tiff")
declare -a SOURCE_PATHS=(
	"/nvme_scratch/source/qat-generated/random-192m.bin"
	"/nvme_scratch/source/qat-generated/mixed-random-zero-192m.bin"
	"/nvme_scratch/source/2021-09-05/Scanned Documents/Image.tif"
)

for source_path in "${SOURCE_PATHS[@]}"; do
	require_file "$source_path"
done

mkdir -p "$OUT_DIR"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"

for media_idx in "${!MEDIA_NAMES[@]}"; do
	media="${MEDIA_NAMES[$media_idx]}"
	bench_root="${MEDIA_ROOTS[$media_idx]}"

	for source_idx in "${!SOURCE_NAMES[@]}"; do
		source_name="${SOURCE_NAMES[$source_idx]}"
		source_path="${SOURCE_PATHS[$source_idx]}"

		for jobs in $JOBS_LIST; do
			out="${OUT_DIR}/zfs-qat-expected-ratio-${expected_ratio}-${media}-${source_name}-jobs${jobs}-${stamp}.csv"
			log="${out%.csv}.log"
			echo "Running expected_ratio=$expected_ratio media=$media source=$source_name jobs=$jobs -> $out" >&2
			BENCH_ROOT="$bench_root" \
			    SOURCE="$source_path" \
			    SOURCE_LABEL="$source_name" \
			    RECORDS="$RECORDS" \
			    MODES="$MODES" \
			    ITERS="$ITERS" \
			    JOBS="$jobs" \
			    VERIFY_MODE="$VERIFY_MODE" \
			    RUN_ORDER="$RUN_ORDER" \
			    OUT="$out" \
			    "$HARNESS" >"$log" 2>&1
		done
	done
done
