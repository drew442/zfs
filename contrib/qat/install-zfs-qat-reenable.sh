#!/bin/sh
set -eu

unit_name="zfs-qat-reenable.service"
source_unit="$(dirname "$0")/$unit_name"
target_unit="/etc/systemd/system/$unit_name"

if [ "$(id -u)" -ne 0 ]; then
	echo "Must be run as root" >&2
	exit 1
fi

if ! command -v systemctl >/dev/null 2>&1; then
	echo "systemctl is required" >&2
	exit 1
fi

if [ ! -r "$source_unit" ]; then
	echo "Missing source unit: $source_unit" >&2
	exit 1
fi

install -m 0644 "$source_unit" "$target_unit"
systemctl daemon-reload
systemctl enable "$unit_name"

echo "Installed and enabled $target_unit"
