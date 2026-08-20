#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Apply every routed AOSP project series to an exact pinned Repo workspace.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ANDROID_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
PROJECTS="$ANDROID_ROOT/patches/projects.tsv"
VERIFIER="$ANDROID_ROOT/tools/verify-aosp-patches.py"
AOSP_SOURCE=

fail()
{
	printf '%s\n' "android/aosp-17/build/apply-aosp-patches.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: android/aosp-17/build/apply-aosp-patches.sh \
       --aosp-source ABSOLUTE_PATH

Run locally on the x86_64 Linux builder after source preparation. Every routed
project must be clean and at the exact revision in the resolved manifest lock.
The command applies only generated per-project patches and performs no fetch,
overlay copy, or transport operation.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--aosp-source)
		[ "$#" -ge 2 ] || fail "--aosp-source requires a path"
		AOSP_SOURCE=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$AOSP_SOURCE" ] || fail "--aosp-source is required"
case "$AOSP_SOURCE" in
/*) ;;
*) fail "AOSP source must be an absolute local path" ;;
esac
case "$AOSP_SOURCE" in
/|*/../*|*/..|*/./*|*/.) fail "AOSP source must be normalized" ;;
esac
[ "$(uname -s)" = Linux ] || fail "builder host must be Linux"
[ "$(uname -m)" = x86_64 ] || fail "builder host must be x86_64"
[ -d "$AOSP_SOURCE/.repo/manifests" ] || fail "AOSP Repo metadata is missing"
[ -x "$VERIFIER" ] || fail "AOSP patch verifier is missing"
[ -f "$PROJECTS" ] || fail "AOSP route table is missing"

"$VERIFIER" --android-root "$ANDROID_ROOT" \
	--aosp-source "$AOSP_SOURCE" --expect base

awk -F '\t' '$1 ~ /^[0-9]+$/ { print $2 "\t" $4 "\t" $11 }' "$PROJECTS" |
while IFS="$(printf '\t')" read -r route_id checkout_path series_dir; do
	checkout="$AOSP_SOURCE/$checkout_path"
	manifest="$ANDROID_ROOT/$series_dir/manifest.tsv"
	awk -F '\t' '$1 ~ /^[0-9]+$/ { print $2 }' "$manifest" |
	while IFS= read -r filename; do
		patch="$ANDROID_ROOT/$series_dir/$filename"
		if ! git -C "$checkout" am "$patch"; then
			git -C "$checkout" am --abort || true
			fail "could not apply $route_id patch: $filename"
		fi
	done
done

"$VERIFIER" --android-root "$ANDROID_ROOT" \
	--aosp-source "$AOSP_SOURCE" --expect final
printf '%s\n' "applied routed AOSP 17 project patches"
