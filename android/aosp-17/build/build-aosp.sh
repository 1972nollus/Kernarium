#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Build the fixed AOSP 17 arm64-only page-agnostic UML product.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ANDROID_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
METADATA="$ANDROID_ROOT/metadata/aosp.env"
VERIFIER="$ANDROID_ROOT/tools/verify-aosp-patches.py"

AOSP_SOURCE=
OUT_DIR=
JOBS=

fail()
{
	printf '%s\n' "android/aosp-17/build/build-aosp.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: android/aosp-17/build/build-aosp.sh \
       --aosp-source ABSOLUTE_PATH \
       --out ABSOLUTE_PATH [--jobs N]

Run locally on the x86_64 Linux builder after applying the routed AOSP patch
series. The output directory must be an ignored directory inside the AOSP Repo
workspace; the pinned Android 17 Soong tree rejects generated host-tool paths
from an external absolute OUT_DIR. Standard Ninja and non-incremental Soong
analysis are fixed producer settings for this source tag.
EOF
}

absolute_path()
{
	local name=$1
	local value=$2
	case "$value" in
	/*) ;;
	*) fail "$name must be an absolute local path" ;;
	esac
	case "$value" in
	/|*/../*|*/..|*/./*|*/.) fail "$name must be normalized" ;;
	esac
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--aosp-source)
		[[ $# -ge 2 ]] || fail "--aosp-source requires a path"
		AOSP_SOURCE=$2
		shift 2
		;;
	--out)
		[[ $# -ge 2 ]] || fail "--out requires a path"
		OUT_DIR=$2
		shift 2
		;;
	--jobs)
		[[ $# -ge 2 ]] || fail "--jobs requires a positive integer"
		JOBS=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[[ -n "$AOSP_SOURCE" ]] || fail "--aosp-source is required"
[[ -n "$OUT_DIR" ]] || fail "--out is required"
absolute_path "AOSP source" "$AOSP_SOURCE"
absolute_path "output directory" "$OUT_DIR"
[[ "$(uname -s)" == Linux ]] || fail "builder host must be Linux"
[[ "$(uname -m)" == x86_64 ]] || fail "builder host must be x86_64"
command -v zip >/dev/null 2>&1 || fail "zip is not available on PATH"
[[ -d "$AOSP_SOURCE/.repo/manifests" ]] || fail "AOSP Repo metadata is missing"
case "$OUT_DIR" in
"$AOSP_SOURCE"/*) ;;
*) fail "output directory must be inside the AOSP workspace for this source tag" ;;
esac
if [[ -z "$JOBS" ]]; then
	JOBS=$(getconf _NPROCESSORS_ONLN)
fi
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "jobs must be a positive integer"
[[ -f "$METADATA" ]] || fail "AOSP metadata is missing"
[[ -x "$VERIFIER" ]] || fail "AOSP patch verifier is missing"
# shellcheck disable=SC1090
source "$METADATA"

"$VERIFIER" --android-root "$ANDROID_ROOT" \
	--aosp-source "$AOSP_SOURCE" --expect final
mkdir -p -- "$OUT_DIR"
OUT_DIR_ABSOLUTE=$OUT_DIR
# This tag's test_package path validation treats absolute generated paths as
# module source paths. The verified suffix names the same in-tree directory
# while keeping Soong's generated paths relative to the Repo root.
OUT_DIR=${OUT_DIR#"$AOSP_SOURCE"/}
export OUT_DIR
export SOONG_NINJA=ninja
export SOONG_INCREMENTAL_ANALYSIS=false

cd "$AOSP_SOURCE"
# shellcheck disable=SC1091
source build/envsetup.sh
lunch "${ANDROID17_PRODUCT}-${ANDROID17_RELEASE_CONFIG}-${ANDROID17_BUILD_VARIANT}"

[[ "$(get_build_var TARGET_PRODUCT)" == "$ANDROID17_PRODUCT" ]] ||
	fail "lunch selected the wrong product"
[[ "$(get_build_var TARGET_ARCH)" == arm64 ]] || fail "product is not arm64"
[[ -z "$(get_build_var TARGET_2ND_ARCH)" ]] || fail "product has a 32-bit secondary ABI"
[[ "$(get_build_var TARGET_MAX_PAGE_SIZE_SUPPORTED)" == "$ANDROID17_PAGE_SIZE" ]] ||
	fail "product maximum page size differs from metadata"
[[ "$(get_build_var TARGET_NO_BIONIC_PAGE_SIZE_MACRO)" == true ]] ||
	fail "product exposes a compile-time bionic page-size macro"
[[ "$(get_build_var TARGET_CHECK_PREBUILT_MAX_PAGE_SIZE)" == true ]] ||
	fail "product does not check prebuilt ELF alignment"
[[ "$(get_build_var TARGET_BOOTS_16K)" == true ]] ||
	fail "product does not select the 16 KiB boot contract"
for variable in \
	PRODUCT_BUILD_SYSTEM_DLKM_IMAGE \
	PRODUCT_BUILD_VENDOR_DLKM_IMAGE \
	PRODUCT_BUILD_ODM_DLKM_IMAGE; do
	[[ "$(get_build_var "$variable")" == false ]] ||
		fail "$variable must be false for the built-in-only kernel"
done
for variable in \
	BOARD_GENERIC_RAMDISK_KERNEL_MODULES \
	BOARD_GENERIC_RAMDISK_KERNEL_MODULES_LOAD \
	BOARD_RECOVERY_KERNEL_MODULES \
	BOARD_VENDOR_RAMDISK_KERNEL_MODULES \
	BOARD_VENDOR_KERNEL_MODULES \
	BOARD_SYSTEM_KERNEL_MODULES \
	BOARD_ODM_KERNEL_MODULES \
	BOARD_KERNEL_MODULES_16K; do
	[[ -z "$(get_build_var "$variable")" ]] ||
		fail "$variable must be empty for the built-in-only kernel"
done

m -j"$JOBS" target-files-package
"$VERIFIER" --android-root "$ANDROID_ROOT" \
	--aosp-source "$AOSP_SOURCE" --expect final
printf '%s\n' "built AOSP 17 UML target files under: $OUT_DIR_ABSOLUTE"
