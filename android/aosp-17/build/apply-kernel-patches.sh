#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Materialize the tracked ACK topic series into a new builder-local worktree.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/android-6.18"
COMMON_ROOT="$ROOT_DIR/kernel/common"
METADATA="$LINE_ROOT/patches/metadata.env"
MANIFEST="$LINE_ROOT/patches/series/manifest.tsv"

ACK_WORKSPACE=
KERNEL_SOURCE=

fail()
{
	printf '%s\n' "android/aosp-17/build/apply-kernel-patches.sh: $*" >&2
	exit 1
}

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

absolute_path()
{
	name=$1
	value=$2
	case "$value" in
	/*) ;;
	*) fail "$name must be an absolute local path" ;;
	esac
	case "$value" in
	/|*/../*|*/..|*/./*|*/.) fail "$name must be normalized" ;;
	esac
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
[ -f "$MANIFEST" ] || fail "missing ACK series manifest: $MANIFEST"
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

"$COMMON_ROOT/tooling/verify-series-policy.py" \
	--line-root "$LINE_ROOT" manifest >/dev/null
git -C "$ACK_COMMON" worktree add --detach \
	"$KERNEL_SOURCE" "$UML_KERNEL_UPSTREAM_COMMIT"

awk -F '\t' '$1 ~ /^[0-9]+$/ { print $2 }' "$MANIFEST" |
while IFS= read -r filename; do
	patch="$LINE_ROOT/patches/series/$filename"
	if ! git -C "$KERNEL_SOURCE" am "$patch"; then
		git -C "$KERNEL_SOURCE" am --abort || true
		fail "could not apply ACK topic: $filename"
	fi
done

topic_count=$(git -C "$KERNEL_SOURCE" rev-list --count \
	"$UML_KERNEL_UPSTREAM_COMMIT..HEAD")
[ "$topic_count" -eq "$UML_KERNEL_PATCH_COUNT" ] ||
	fail "replayed $topic_count topics, expected $UML_KERNEL_PATCH_COUNT"
[ -z "$(git -C "$KERNEL_SOURCE" rev-list --merges \
	"$UML_KERNEL_UPSTREAM_COMMIT..HEAD")" ] || fail "replayed series has a merge"
final_tree=$(git -C "$KERNEL_SOURCE" rev-parse HEAD^{tree})
[ "$final_tree" = "$UML_KERNEL_FINAL_TREE" ] ||
	fail "replayed tree is $final_tree, expected $UML_KERNEL_FINAL_TREE"
[ -z "$(git -C "$KERNEL_SOURCE" status --porcelain)" ] ||
	fail "replayed ACK worktree is dirty"

"$COMMON_ROOT/tooling/verify-series-policy.py" \
	--line-root "$LINE_ROOT" topology \
	--source "$KERNEL_SOURCE" --base "$UML_KERNEL_UPSTREAM_COMMIT" \
	--tip HEAD >/dev/null
"$COMMON_ROOT/abi/sync.sh" --kernel-metadata "$METADATA" \
	--kernel-source "$KERNEL_SOURCE" --verify >/dev/null
printf '%s\n' "materialized Android ACK source: $KERNEL_SOURCE"
