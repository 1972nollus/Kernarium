#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run one guest campaign only after the EROFS image verifies, then
# verify it again even when the campaign fails.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ROOTFS=${UML_INTEGRATION_ROOTFS:-"$ROOT_DIR/rootfs.erofs"}
MANIFEST=${UML_ROOTFS_MANIFEST:-"$ROOT_DIR/tools/rootfs/manifest.json"}

usage()
{
	printf '%s\n' "Usage: tools/rootfs/with-verified-rootfs.sh [--rootfs PATH] [--manifest PATH] -- COMMAND [ARG ...]" >&2
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--rootfs)
		[ "$#" -ge 2 ] || { usage; exit 2; }
		ROOTFS=$2
		shift 2
		;;
	--manifest)
		[ "$#" -ge 2 ] || { usage; exit 2; }
		MANIFEST=$2
		shift 2
		;;
	--)
		shift
		break
		;;
	*)
		usage
		exit 2
		;;
	esac
done
[ "$#" -gt 0 ] || { usage; exit 2; }

exec python3 "$ROOT_DIR/tools/rootfs/run_verified.py" \
	--image "$ROOTFS" --manifest "$MANIFEST" -- "$@"
