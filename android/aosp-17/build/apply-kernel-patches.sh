#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Materialize the tracked ACK topic series into a new builder-local worktree.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/android-6.18"
COMMON_ROOT="$ROOT_DIR/kernel/common"
METADATA="$LINE_ROOT/patches/metadata.env"
KERNEL_SERIES="$COMMON_ROOT/tooling/kernel_series.py"

ACK_WORKSPACE=
KERNEL_SOURCE=

# shellcheck disable=SC1090
. "$SCRIPT_DIR/common.sh"

usage()
{
	cat <<'EOF'
Usage: android/aosp-17/build/apply-kernel-patches.sh \
       --ack-workspace ABSOLUTE_PATH \
       --kernel-source NEW_ABSOLUTE_PATH

Run locally in the reviewed outer-project copy on the x86_64 Linux builder.
The destination must not exist. The command creates a detached Git worktree
from the pinned ACK common checkout and applies the generated local patch
series. It performs no source fetch and no transport operation.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--ack-workspace)
		[ "$#" -ge 2 ] || fail "--ack-workspace requires a path"
		ACK_WORKSPACE=$2
		shift 2
		;;
	--kernel-source)
		[ "$#" -ge 2 ] || fail "--kernel-source requires a path"
		KERNEL_SOURCE=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$ACK_WORKSPACE" ] || fail "--ack-workspace is required"
[ -n "$KERNEL_SOURCE" ] || fail "--kernel-source is required"
absolute_path "ACK workspace" "$ACK_WORKSPACE"
absolute_path "kernel source" "$KERNEL_SOURCE"
[ "$(uname -s)" = Linux ] || fail "builder host must be Linux"
[ "$(uname -m)" = x86_64 ] || fail "builder host must be x86_64"
[ -f "$METADATA" ] || fail "missing ACK metadata: $METADATA"
# shellcheck disable=SC1090
. "$METADATA"

ACK_COMMON="$ACK_WORKSPACE/common"
[ -e "$ACK_COMMON/.git" ] || fail "ACK common checkout is missing: $ACK_COMMON"
[ ! -e "$KERNEL_SOURCE" ] && [ ! -L "$KERNEL_SOURCE" ] ||
	fail "kernel source destination already exists: $KERNEL_SOURCE"
KERNEL_PARENT=$(dirname -- "$KERNEL_SOURCE")
[ -d "$KERNEL_PARENT" ] && [ -w "$KERNEL_PARENT" ] ||
	fail "kernel source parent must exist and be writable: $KERNEL_PARENT"

actual_commit=$(git -C "$ACK_COMMON" rev-parse HEAD)
[ "$actual_commit" = "$UML_KERNEL_UPSTREAM_COMMIT" ] ||
	fail "ACK common is $actual_commit, expected $UML_KERNEL_UPSTREAM_COMMIT"
actual_tree=$(git -C "$ACK_COMMON" rev-parse HEAD^{tree})
[ "$actual_tree" = "$UML_KERNEL_UPSTREAM_TREE" ] ||
	fail "ACK common tree is $actual_tree, expected $UML_KERNEL_UPSTREAM_TREE"
[ -z "$(git -C "$ACK_COMMON" status --porcelain)" ] ||
	fail "ACK common checkout is dirty"

python3 -B "$KERNEL_SERIES" check --metadata "$METADATA" ||
	fail "the ACK series files are invalid"
git -C "$ACK_COMMON" worktree add --detach \
	"$KERNEL_SOURCE" "$UML_KERNEL_UPSTREAM_COMMIT"

# Every worktree of the ACK checkout shares its branches, so the replay stays
# detached and creates none.
python3 -B "$KERNEL_SERIES" replay --metadata "$METADATA" \
	--source "$KERNEL_SOURCE" --detached >/dev/null ||
	fail "could not replay the ACK series onto $UML_KERNEL_UPSTREAM_COMMIT"
python3 -B "$KERNEL_SERIES" verify-branch --metadata "$METADATA" \
	--source "$KERNEL_SOURCE" --tip HEAD --detached ||
	fail "replayed ACK worktree is not the checked-in series"
[ -z "$(git -C "$KERNEL_SOURCE" status --porcelain)" ] ||
	fail "replayed ACK worktree is dirty"

"$COMMON_ROOT/abi/sync.sh" --kernel-source "$KERNEL_SOURCE" --verify >/dev/null
printf '%s\n' "materialized Android ACK source: $KERNEL_SOURCE"
