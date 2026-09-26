#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Regenerate the checked-in series of one mainline slice from its managed
# topic branch. The x86_64 overlay is first rebased onto the current arm64
# branch, so an arm64 change followed by both slices keeps them consistent.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
KERNEL_SERIES="$ROOT_DIR/kernel/common/tooling/kernel_series.py"
SOURCE_LOCATION="$LINE_ROOT/scripts/source-location.sh"
SLICE_SELECTOR="$LINE_ROOT/scripts/select-slice.sh"
GIT_IDENTITY="$ROOT_DIR/scripts/git-identity.sh"
REQUESTED_SOURCE=
SLICE=arm64

fail()
{
	printf '%s\n' "kernel/mainline-6.18/scripts/rebase-patches.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'USAGE'
Usage: kernel/mainline-6.18/scripts/rebase-patches.sh \
       [--slice arm64|x86_64] [--source CONFIGURED_PATH]

Amend, insert, or reorder topic commits on the slice's managed branch, then
run this command to export its series and record its final tree. Never edit
generated .patch files.
USAGE
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--slice)
		[ "$#" -ge 2 ] || fail "--slice requires arm64 or x86_64"
		SLICE=$2
		shift 2
		;;
	--source)
		[ "$#" -ge 2 ] || fail "--source requires a path"
		REQUESTED_SOURCE=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -r "$SLICE_SELECTOR" ] || fail "missing slice selector: $SLICE_SELECTOR"
[ -r "$GIT_IDENTITY" ] || fail "missing Git identity helper: $GIT_IDENTITY"
# shellcheck disable=SC1090
. "$GIT_IDENTITY"
uml_require_git_identity 'kernel/mainline-6.18/scripts/rebase-patches.sh'
# shellcheck disable=SC1090
. "$SLICE_SELECTOR"
uml_kernel_select_slice "$LINE_ROOT" "$SLICE" ||
	fail "cannot select kernel slice: $SLICE"

[ -x "$SOURCE_LOCATION" ] || fail "missing source-location verifier: $SOURCE_LOCATION"
SOURCE=$("$SOURCE_LOCATION" --require-source) ||
	fail "kernel source configuration is invalid"
if [ -n "$REQUESTED_SOURCE" ] && [ "$REQUESTED_SOURCE" != "$SOURCE" ]; then
	fail "--source must match configured UML_KERNEL_SOURCE_DIR: $SOURCE"
fi
[ -d "$SOURCE/.git" ] || fail "source is not a Git working tree: $SOURCE"

kernel_series()
{
	python3 -B "$KERNEL_SERIES" "$@" --metadata "$UML_KERNEL_METADATA_PATH" \
		--source "$SOURCE"
}

if [ "$UML_KERNEL_SLICE" = x86_64 ]; then
	kernel_series refresh >/dev/null
fi
kernel_series regenerate >/dev/null
printf '%s\n' "regenerated the $UML_KERNEL_SLICE kernel series from $UML_KERNEL_PATCH_BRANCH"
