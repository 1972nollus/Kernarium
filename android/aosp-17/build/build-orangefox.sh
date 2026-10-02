#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the kernel-less OrangeFox recovery partition for Darwin UML.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ANDROID_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
METADATA="$ANDROID_ROOT/third_party/orangefox/metadata.env"
SOURCE_VERIFIER="$ANDROID_ROOT/tools/verify-orangefox-source.py"
IMAGE_VERIFIER="$ANDROID_ROOT/tools/verify-orangefox-recovery.py"
KERNEL_VERIFIER="$SCRIPT_DIR/verify-kernel-elf.py"

ORANGEFOX_SOURCE=
OUT_DIR=
KERNEL_ELF=
OUTPUT=
JOBS=

# shellcheck disable=SC1090
source "$SCRIPT_DIR/common.sh"

usage()
{
	cat <<'EOF'
Usage: android/aosp-17/build/build-orangefox.sh \
       --orangefox-source ABSOLUTE_PATH \
       --out ABSOLUTE_PATH \
       --kernel-elf ABSOLUTE_PATH \
       --output ABSOLUTE_PATH [--jobs N]

Build the one supported OrangeFox recovery image on the x86_64 Linux builder.
The output directory must be inside the OrangeFox Repo workspace. The ACK ELF
is copied only as a build-time VINTF input; the verified boot-image-v4 result
has a zero-byte kernel section. An existing output image is never replaced.
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--orangefox-source)
		[[ $# -ge 2 ]] || fail "--orangefox-source requires a path"
		ORANGEFOX_SOURCE=$2
		shift 2
		;;
	--out)
		[[ $# -ge 2 ]] || fail "--out requires a path"
		OUT_DIR=$2
		shift 2
		;;
	--kernel-elf)
		[[ $# -ge 2 ]] || fail "--kernel-elf requires a path"
		KERNEL_ELF=$2
		shift 2
		;;
	--output)
		[[ $# -ge 2 ]] || fail "--output requires a path"
		OUTPUT=$2
		shift 2
		;;
	--jobs)
		[[ $# -ge 2 ]] || fail "--jobs requires a value"
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

for value in ORANGEFOX_SOURCE OUT_DIR KERNEL_ELF OUTPUT; do
	[[ -n "${!value}" ]] || fail "--${value,,} is required"
done
absolute_path "OrangeFox source" "$ORANGEFOX_SOURCE"
absolute_path "output directory" "$OUT_DIR"
absolute_path "kernel ELF" "$KERNEL_ELF"
absolute_path "output image" "$OUTPUT"
[[ "$(uname -s)" == Linux ]] || fail "builder host must be Linux"
[[ "$(uname -m)" == x86_64 ]] || fail "builder host must be x86_64"
[[ -d "$ORANGEFOX_SOURCE/.repo/manifests" ]] || fail "OrangeFox Repo metadata is missing"
[[ -f "$KERNEL_ELF" && ! -L "$KERNEL_ELF" ]] || fail "kernel ELF is missing or unsafe"
[[ ! -e "$OUTPUT" && ! -L "$OUTPUT" ]] || fail "output already exists: $OUTPUT"
case "$OUT_DIR" in
"$ORANGEFOX_SOURCE"/*) ;;
*) fail "output directory must be inside the OrangeFox workspace" ;;
esac
case "$OUTPUT" in
"$ORANGEFOX_SOURCE"/*) fail "final output must be outside the OrangeFox source tree" ;;
esac
if [[ -z "$JOBS" ]]; then
	JOBS=$(getconf _NPROCESSORS_ONLN)
fi
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "jobs must be a positive integer"
for tool in "$SOURCE_VERIFIER" "$IMAGE_VERIFIER" "$KERNEL_VERIFIER"; do
	[[ -x "$tool" ]] || fail "required verifier is missing: $tool"
done
[[ -f "$METADATA" ]] || fail "OrangeFox metadata is missing"
# shellcheck disable=SC1090
source "$METADATA"

"$SOURCE_VERIFIER" --android-root "$ANDROID_ROOT" \
	--orangefox-source "$ORANGEFOX_SOURCE" --expect final \
	--allow-prebuilt-kernel
"$KERNEL_VERIFIER" "$KERNEL_ELF"
install -D -m 0755 -- "$KERNEL_ELF" \
	"$ORANGEFOX_SOURCE/$ORANGEFOX_DEVICE_OVERLAY_DESTINATION/prebuilt/kernel"
"$SOURCE_VERIFIER" --android-root "$ANDROID_ROOT" \
	--orangefox-source "$ORANGEFOX_SOURCE" --expect final \
	--allow-prebuilt-kernel

mkdir -p -- "$OUT_DIR/kernarium-tools"
ln -sfn -- "$(command -v python3)" "$OUT_DIR/kernarium-tools/python"
OUT_DIR_ABSOLUTE=$OUT_DIR
OUT_DIR=${OUT_DIR#"$ORANGEFOX_SOURCE"/}
export OUT_DIR
export PATH="$OUT_DIR_ABSOLUTE/kernarium-tools:$PATH"
export LC_ALL=C
export FOX_BUILD_DEVICE=darwin

cd "$ORANGEFOX_SOURCE"
# shellcheck disable=SC1091
set +u
source build/envsetup.sh
# envsetup clears variables remembered by an earlier build invocation. Apply the
# checked device policy after it and before lunch snapshots the Soong environment.
# shellcheck disable=SC1091
source "$ORANGEFOX_SOURCE/$ORANGEFOX_DEVICE_OVERLAY_DESTINATION/vendorsetup.sh"
lunch "${ORANGEFOX_PRODUCT}-${ORANGEFOX_RELEASE_CONFIG}-eng"

[[ "$(get_build_var TARGET_PRODUCT)" == "$ORANGEFOX_PRODUCT" ]] ||
	fail "lunch selected the wrong product"
[[ "$(get_build_var TARGET_ARCH)" == arm64 ]] || fail "OrangeFox product is not arm64"
[[ -z "$(get_build_var TARGET_2ND_ARCH)" ]] || fail "OrangeFox product has a secondary ABI"
[[ "$(get_build_var BOARD_BOOT_HEADER_VERSION)" == 4 ]] ||
	fail "OrangeFox boot header is not version 4"
[[ "$(get_build_var BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE)" == true ]] ||
	fail "OrangeFox recovery would embed the ACK kernel"
[[ "$(get_build_var BOARD_RECOVERYIMAGE_PARTITION_SIZE)" == 33554432 ]] ||
	fail "OrangeFox partition size differs from the fixed GPT"
[[ "$(get_build_var TARGET_MAX_PAGE_SIZE_SUPPORTED)" == 65536 ]] ||
	fail "OrangeFox userspace is not page-size agnostic through 64 KiB"

# Recovery's assembled ramdisk is install staging, not a dependency-tracked
# view of every rebuilt shared library. Refresh it so an incremental source
# patch cannot leave an older library sealed into recovery.img.
m installclean
m -j"$JOBS" recoveryimage
set -u
RECOVERY_IMAGE="$OUT_DIR_ABSOLUTE/target/product/darwin/recovery.img"
[[ -f "$RECOVERY_IMAGE" && ! -L "$RECOVERY_IMAGE" ]] ||
	fail "OrangeFox recovery image was not produced"
"$IMAGE_VERIFIER" --android-root "$ANDROID_ROOT" --image "$RECOVERY_IMAGE"
mkdir -p -- "$(dirname -- "$OUTPUT")"
install -m 0644 -- "$RECOVERY_IMAGE" "$OUTPUT"
"$IMAGE_VERIFIER" --android-root "$ANDROID_ROOT" --image "$OUTPUT"
printf '%s\n' "built OrangeFox recovery partition: $OUTPUT"
