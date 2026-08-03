#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Report patch-series conflicts against a caller-selected development ref.
# This opt-in audit uses a temporary Git clone because arbitrary future refs do
# not have release archives. It never supplies source to a build or rewrites a
# managed packaged-release checkout.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
METADATA="$ROOT_DIR/kernel/patches/metadata.env"
MANIFEST="$ROOT_DIR/kernel/patches/series/manifest.tsv"

fail()
{
	printf '%s\n' "kernel/check-next-version.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/check-next-version.sh --work-root PATH --ref REF [--keep]

Clone the audit-only Linux origin, detach at REF, and apply the complete tracked
series with git am --3way. A conflict is a failing result for scheduled CI.
EOF
}

WORK_ROOT=''
UPSTREAM_REF=''
KEEP=0
while [ "$#" -gt 0 ]; do
	case "$1" in
	--work-root)
		[ "$#" -ge 2 ] || fail "--work-root requires a path"
		WORK_ROOT=$2
		shift 2
		;;
	--ref)
		[ "$#" -ge 2 ] || fail "--ref requires a value"
		UPSTREAM_REF=$2
		shift 2
		;;
	--keep)
		KEEP=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$WORK_ROOT" ] || fail "--work-root is required"
[ -n "$UPSTREAM_REF" ] || fail "--ref is required"
[ -r "$METADATA" ] || fail "missing metadata: $METADATA"
[ -r "$MANIFEST" ] || fail "missing manifest: $MANIFEST"
# shellcheck disable=SC1090
. "$METADATA"

mkdir -p -- "$WORK_ROOT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
CHECK_ROOT=$(mktemp -d "$WORK_ROOT/uml-linux-next.XXXXXX") ||
	fail "cannot create temporary checkout"
cleanup()
{
	if [ "$KEEP" -eq 0 ]; then
		rm -rf -- "$CHECK_ROOT"
	fi
}
trap cleanup 0 HUP INT TERM

git clone --filter=blob:none --no-checkout "$UML_KERNEL_NEXT_VERSION_ORIGIN" \
	"$CHECK_ROOT/linux"
git -C "$CHECK_ROOT/linux" fetch --no-tags origin "$UPSTREAM_REF"
git -C "$CHECK_ROOT/linux" switch --detach FETCH_HEAD

tab=$(printf '\t')
while IFS="$tab" read -r order filename patch_id digest subject; do
	case "$order" in \#*|order|'') continue ;; esac
	patch="$ROOT_DIR/kernel/patches/series/$filename"
	[ -r "$patch" ] || fail "missing patch: $filename"
	if ! git -C "$CHECK_ROOT/linux" am --3way "$patch"; then
		git -C "$CHECK_ROOT/linux" am --abort >/dev/null 2>&1 || true
		fail "patch $order ($filename) conflicts with $UPSTREAM_REF"
	fi
done < "$MANIFEST"

printf '%s\n' "complete series applies to $UPSTREAM_REF"
if [ "$KEEP" -eq 1 ]; then
	trap - 0 HUP INT TERM
	printf '%s\n' "next-version checkout retained: $CHECK_ROOT/linux"
fi
