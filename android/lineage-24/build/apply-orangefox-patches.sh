#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Replay the two local OrangeFox integration topics and sync the device tree.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ANDROID_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
ROOT_DIR=$(CDPATH= cd -- "$ANDROID_ROOT/../.." && pwd)
PATCH_SERIES="$ROOT_DIR/scripts/patch_series.py"
PROJECTS="$ANDROID_ROOT/third_party/orangefox/patches/projects.tsv"
VERIFIER="$ANDROID_ROOT/tools/verify-orangefox-source.py"
DEVICE_SYNC="$ANDROID_ROOT/tools/sync-device-tree.py"
ORANGEFOX_SOURCE=

# shellcheck disable=SC1090
. "$SCRIPT_DIR/common.sh"

usage()
{
	cat <<'EOF'
Usage: android/lineage-24/build/apply-orangefox-patches.sh \
       --orangefox-source ABSOLUTE_PATH

Run on the x86_64 Linux builder after prepare-orangefox-source.sh. Routed Repo
projects must be clean and at their exact bases. This command performs no
fetch: it applies the generated topics and synchronizes only the checked
Kernarium OrangeFox device tree to device/kernarium/darwin.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--orangefox-source)
		[ "$#" -ge 2 ] || fail "--orangefox-source requires a path"
		ORANGEFOX_SOURCE=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$ORANGEFOX_SOURCE" ] || fail "--orangefox-source is required"
absolute_path "OrangeFox source" "$ORANGEFOX_SOURCE"
[ "$(uname -s)" = Linux ] || fail "builder host must be Linux"
[ "$(uname -m)" = x86_64 ] || fail "builder host must be x86_64"
[ -d "$ORANGEFOX_SOURCE/.repo/manifests" ] || fail "OrangeFox Repo metadata is missing"
[ -f "$PROJECTS" ] || fail "OrangeFox route table is missing"
[ -x "$VERIFIER" ] || fail "OrangeFox source verifier is missing"
[ -x "$DEVICE_SYNC" ] || fail "device-tree sync tool is missing"

"$VERIFIER" --android-root "$ANDROID_ROOT"
"$VERIFIER" --android-root "$ANDROID_ROOT" \
	--orangefox-source "$ORANGEFOX_SOURCE" --expect base

awk -F '\t' '$1 ~ /^[0-9]+$/ { print $2 "\t" $4 "\t" $8 "\t" $9 }' "$PROJECTS" |
while IFS="$(printf '\t')" read -r route_id checkout_path final_tree series_dir; do
	python3 -B "$PATCH_SERIES" apply --repo "$ORANGEFOX_SOURCE/$checkout_path" \
		--series-dir "$ANDROID_ROOT/$series_dir" \
		--expect-tree "$final_tree" >/dev/null ||
		fail "could not apply the $route_id series"
done

"$DEVICE_SYNC" --android-root "$ANDROID_ROOT" --tree orangefox \
	--workspace "$ORANGEFOX_SOURCE"
"$VERIFIER" --android-root "$ANDROID_ROOT" \
	--orangefox-source "$ORANGEFOX_SOURCE" --expect final
printf '%s\n' "applied routed OrangeFox patches and device tree"
