#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fresh pinned-commit replay verification. This never points at ./linux, so a
# passing result proves the exact upstream Git input and tracked patch series
# rather than a developer's pre-existing managed branch.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"

fail()
{
	printf '%s\n' "kernel/mainline-6.18/scripts/verify-replay.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/mainline-6.18/scripts/verify-replay.sh \
       --work-root PATH [--keep] [--skip-build] [--jobs N]

PATH must be on a case-sensitive filesystem. The command materializes the
exact pinned stable-branch commit there, validates its final tree, and by
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

# The replay has its own disposable source configuration. It remains outside
# the outer Git repository and is validated by the same case-sensitivity gate
# as the maintained checkout.
UML_KERNEL_SOURCE_CONFIG="$REPLAY_DIR/source-location.conf"
export UML_KERNEL_SOURCE_CONFIG
printf 'UML_KERNEL_SOURCE_DIR=%s\n' "$REPLAY_DIR/linux" \
	> "$UML_KERNEL_SOURCE_CONFIG"

"$LINE_ROOT/scripts/prepare-source.sh"
"$LINE_ROOT/scripts/prepare-source.sh" --verify --no-fetch
"$LINE_ROOT/scripts/audit-delta.sh" --tree HEAD

if [ "$BUILD" -eq 1 ]; then
	if [ -n "$JOBS" ]; then
		"$LINE_ROOT/build/build-kernel.sh" \
			--build "$REPLAY_DIR/build-16k" --page-size 16k --jobs "$JOBS"
		"$LINE_ROOT/build/build-kernel.sh" \
			--build "$REPLAY_DIR/build-4k" --page-size 4k --jobs "$JOBS"
	else
		"$LINE_ROOT/build/build-kernel.sh" \
			--build "$REPLAY_DIR/build-16k" --page-size 16k
		"$LINE_ROOT/build/build-kernel.sh" \
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
