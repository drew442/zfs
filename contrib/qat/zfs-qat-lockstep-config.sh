#!/bin/sh
set -eu

modprobe_conf="${ZFS_QAT_MODPROBE_CONF:-/etc/modprobe.d/zfs-qat.conf}"

usage() {
	cat <<'USAGE'
Usage:
  zfs-qat-lockstep-config.sh show
  zfs-qat-lockstep-config.sh apply --completion interrupt|poll [--recordsize BYTES] [--decompress-disable profile|0|1] [--update-initramfs]
  zfs-qat-lockstep-config.sh restore-default [--update-initramfs]

Environment:
  QAT_CONF_FILES             Space-separated QAT config files to edit.
  ZFS_QAT_MODPROBE_CONF      ZFS module config path. Default: /etc/modprobe.d/zfs-qat.conf

Notes:
  Completion mode is a boot-time lock-step setting. QAT DcNIsPolled values and
  zfs_qat_dc_poll must match before ZFS initializes QAT DC.
USAGE
}

die() {
	echo "$*" >&2
	exit 1
}

require_root() {
	[ "$(id -u)" -eq 0 ] || die "Must be run as root"
}

qat_conf_files() {
	if [ "${QAT_CONF_FILES:-}" ]; then
		# QAT_CONF_FILES is intentionally a simple space-separated override.
		# shellcheck disable=SC2086
		printf '%s\n' $QAT_CONF_FILES
		return
	fi

	for conf in /etc/dh895xcc_dev*.conf /etc/c6xx_dev*.conf; do
		[ -e "$conf" ] && printf '%s\n' "$conf"
	done
}

backup_file() {
	file="$1"
	stamp="$2"

	[ -e "$file" ] || return 0
	cp -a "$file" "$file.pre-zfs-qat-lockstep-$stamp"
}

validate_uint() {
	value="$1"
	name="$2"

	case "$value" in
		''|*[!0-9]*)
			die "$name must be an unsigned integer"
			;;
	esac
}

poll_value_for_completion() {
	case "$1" in
		interrupt) printf '0' ;;
		poll) printf '1' ;;
		*) die "Invalid completion mode: $1" ;;
	esac
}

set_qat_poll_mode() {
	poll_value="$1"
	stamp="$2"
	found=0

	for conf in $(qat_conf_files); do
		[ -e "$conf" ] || continue
		found=1
		backup_file "$conf" "$stamp"
		tmp="${conf}.zfs-qat-lockstep.$$"
		awk -v poll="$poll_value" '
		    /^\[KERNEL_QAT\]/ {
			in_kernel = 1
			print
			next
		    }
		    /^\[/ {
			in_kernel = 0
			print
			next
		    }
		    in_kernel && $1 ~ /^Dc[0-9]+IsPolled$/ {
			print $1 " = " poll
			next
		    }
		    { print }
		' "$conf" > "$tmp"
		cat "$tmp" > "$conf"
		rm -f "$tmp"
	done

	[ "$found" -eq 1 ] || die "No QAT config files found"
}

write_zfs_modprobe() {
	poll_value="$1"
	recordsize="$2"
	decompress_disable="$3"
	stamp="$4"
	line="options zfs zfs_qat_compress_disable=0 zfs_qat_checksum_disable=1 zfs_qat_encrypt_disable=1 zfs_qat_cpa_dc_level=profile zfs_qat_dc_max_buf_size=profile zfs_qat_dc_poll=$poll_value"

	if [ "$recordsize" ]; then
		validate_uint "$recordsize" "--recordsize"
		line="$line zfs_qat_dc_profile_recordsize=$recordsize"
	fi

	if [ "$decompress_disable" ]; then
		case "$decompress_disable" in
			profile|0|1) ;;
			*) die "--decompress-disable must be profile, 0, or 1" ;;
		esac
		line="$line zfs_qat_decompress_disable=$decompress_disable"
	fi

	backup_file "$modprobe_conf" "$stamp"
	printf '%s\n' "$line" > "$modprobe_conf"
}

write_default_modprobe() {
	stamp="$1"

	backup_file "$modprobe_conf" "$stamp"
	printf '%s\n' "options zfs zfs_qat_compress_disable=0 zfs_qat_checksum_disable=1 zfs_qat_encrypt_disable=1 zfs_qat_cpa_dc_level=profile zfs_qat_dc_max_buf_size=profile" > "$modprobe_conf"
}

run_update_initramfs() {
	if ! command -v update-initramfs >/dev/null 2>&1; then
		die "update-initramfs is required; dracut is intentionally not used"
	fi

	update-initramfs -u -k "$(uname -r)"
}

show_config() {
	echo "ZFS module config: $modprobe_conf"
	if [ -r "$modprobe_conf" ]; then
		cat "$modprobe_conf"
	else
		echo "(missing)"
	fi

	for conf in $(qat_conf_files); do
		[ -r "$conf" ] || continue
		echo
		echo "QAT config: $conf"
		awk '
		    /^\[KERNEL_QAT\]/ {
			in_kernel = 1
			print
			next
		    }
		    /^\[/ {
			in_kernel = 0
		    }
		    in_kernel && $1 ~ /^Dc[0-9]+IsPolled$/ {
			print
		    }
		' "$conf"
	done
}

apply_config() {
	completion=""
	recordsize=""
	decompress_disable=""
	update_initramfs=0

	while [ "$#" -gt 0 ]; do
		case "$1" in
			--completion)
				[ "$#" -ge 2 ] || die "Missing value for --completion"
				completion="$2"
				shift 2
				;;
			--recordsize)
				[ "$#" -ge 2 ] || die "Missing value for --recordsize"
				recordsize="$2"
				shift 2
				;;
			--decompress-disable)
				[ "$#" -ge 2 ] || die "Missing value for --decompress-disable"
				decompress_disable="$2"
				shift 2
				;;
			--update-initramfs)
				update_initramfs=1
				shift
				;;
			-h|--help)
				usage
				exit 0
				;;
			*)
				die "Unknown argument: $1"
				;;
		esac
	done

	[ "$completion" ] || die "--completion is required"
	poll_value="$(poll_value_for_completion "$completion")"
	stamp="$(date -u +%Y%m%dT%H%M%SZ)"

	set_qat_poll_mode "$poll_value" "$stamp"
	write_zfs_modprobe "$poll_value" "$recordsize" "$decompress_disable" "$stamp"

	if [ "$update_initramfs" -eq 1 ]; then
		run_update_initramfs
	fi

	echo "Applied $completion completion mode. Reboot before relying on the new mode."
}

restore_default() {
	update_initramfs=0

	while [ "$#" -gt 0 ]; do
		case "$1" in
			--update-initramfs)
				update_initramfs=1
				shift
				;;
			-h|--help)
				usage
				exit 0
				;;
			*)
				die "Unknown argument: $1"
				;;
		esac
	done

	stamp="$(date -u +%Y%m%dT%H%M%SZ)"
	set_qat_poll_mode 0 "$stamp"
	write_default_modprobe "$stamp"

	if [ "$update_initramfs" -eq 1 ]; then
		run_update_initramfs
	fi

	echo "Restored default interrupt completion mode. Reboot before relying on the new mode."
}

cmd="${1:-}"
case "$cmd" in
	show)
		shift
		[ "$#" -eq 0 ] || die "show does not accept arguments"
		show_config
		;;
	apply)
		shift
		require_root
		apply_config "$@"
		;;
	restore-default)
		shift
		require_root
		restore_default "$@"
		;;
	-h|--help|'')
		usage
		;;
	*)
		usage >&2
		exit 1
		;;
esac
