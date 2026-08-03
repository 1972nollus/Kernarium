#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fresh packaged-release replay verification. This never points at ./linux, so
# a passing result proves the authenticated archive and tracked patch series
# rather than a developer's pre-existing managed branch.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

fail()
{
	printf '%s\n' "kernel/verify-replay.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/verify-replay.sh --work-root PATH [--keep] [--skip-build]
                               [--jobs N]

PATH must be on a case-sensitive filesystem. The command materializes a fresh
authenticated official release there, validates its exact final tree, and by
default builds both 16 KiB and 4 KiB configurations out of tree.
EOF
}

WORK_ROOT=${UML_KERNEL_REPLAY_ROOT:-}
KEEP=0
BUILD=1
JOBS=''
while [ "$#" -gt 0 ]; do
	case "$1" in
	--work-root)
		[ "$#" -ge 2 ] || fail "--work-root requires a path"
		WORK_ROOT=$2
		shift 2
		;;
	--keep)
		KEEP=1
		shift
		;;
	--skip-build)
		BUILD=0
		shift
		;;
	--jobs)
		[ "$#" -ge 2 ] || fail "--jobs requires a positive integer"
		JOBS=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		fail "unknown option: $1"
		;;
	esac
done

[ -n "$WORK_ROOT" ] || {
	usage >&2
	exit 2
}
mkdir -p -- "$WORK_ROOT"
[ -d "$WORK_ROOT" ] || fail "cannot create work root: $WORK_ROOT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
REPLAY_DIR=$(mktemp -d "$WORK_ROOT/uml-linux-replay.XXXXXX") ||
	fail "cannot create a replay directory below $WORK_ROOT"

cleanup()
{
	if [ "$KEEP" -eq 0 ]; then
		rm -rf -- "$REPLAY_DIR"
	fi
}
trap cleanup 0 HUP INT TERM

# Deliberately pass arguments without eval so a caller-controlled work root is
# never reparsed as shell source.
"$ROOT_DIR/kernel/prepare-source.sh" --source "$REPLAY_DIR/linux"
"$ROOT_DIR/kernel/prepare-source.sh" --source "$REPLAY_DIR/linux" --verify --no-fetch
"$ROOT_DIR/kernel/audit-delta.sh" --source "$REPLAY_DIR/linux" --tree HEAD

if [ "$BUILD" -eq 1 ]; then
	if [ -n "$JOBS" ]; then
		"$ROOT_DIR/kernel/build-kernel.sh" --source "$REPLAY_DIR/linux" \
			--build "$REPLAY_DIR/build-16k" --page-size 16k --jobs "$JOBS"
		"$ROOT_DIR/kernel/build-kernel.sh" --source "$REPLAY_DIR/linux" \
			--build "$REPLAY_DIR/build-4k" --page-size 4k --jobs "$JOBS"
	else
		"$ROOT_DIR/kernel/build-kernel.sh" --source "$REPLAY_DIR/linux" \
			--build "$REPLAY_DIR/build-16k" --page-size 16k
		"$ROOT_DIR/kernel/build-kernel.sh" --source "$REPLAY_DIR/linux" \
			--build "$REPLAY_DIR/build-4k" --page-size 4k
	fi
fi

printf '%s\n' "fresh replay verified in $REPLAY_DIR"
if [ "$KEEP" -eq 0 ]; then
	printf '%s\n' "replay directory will be removed after this command"
else
	trap - 0 HUP INT TERM
	printf '%s\n' "replay directory retained: $REPLAY_DIR"
fi
