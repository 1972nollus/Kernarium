#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Assemble the verified static Android partition images into one raw UBD disk.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ANDROID_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
LAYOUT="$ANDROID_ROOT/support/image/partition-layout.json"
TARGET_FILES_VERIFIER="$ANDROID_ROOT/tools/verify-target-files.py"
DISK_TOOL="$ANDROID_ROOT/tools/ubd-disk.py"
BOOT_IMAGE_TOOL="$ANDROID_ROOT/tools/boot-image.py"
INITRAMFS_VERIFIER="$ANDROID_ROOT/tools/verify-initramfs.py"

TARGET_FILES=
KERNEL_ELF=
RAMDISK=
MKBOOTIMG=
OUTPUT=
WORK_DIR=

fail()
{
	printf '%s\n' "android/aosp-17/build/assemble-image.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: android/aosp-17/build/assemble-image.sh \
       --target-files ABSOLUTE_PATH \
       --kernel-elf ABSOLUTE_PATH \
       --ramdisk ABSOLUTE_PATH \
       --mkbootimg ABSOLUTE_PATH \
       --output ABSOLUTE_PATH

Run locally on the x86_64 Linux builder after build-aosp.sh. The target-files
staging directory must remain beside its .zip archive. The raw normal ramdisk
is wrapped in a kernelless boot-image v4 init_boot partition, and the AOSP
recovery image is embedded as the recovery partition. The output is a new raw
GPT disk; an existing path is never replaced.
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

cleanup()
{
	if [[ -n "$WORK_DIR" && -d "$WORK_DIR" ]]; then
		rm -rf -- "$WORK_DIR"
	fi
}
trap cleanup EXIT

while [[ $# -gt 0 ]]; do
	case "$1" in
	--target-files)
		[[ $# -ge 2 ]] || fail "--target-files requires a path"
		TARGET_FILES=$2
		shift 2
		;;
	--kernel-elf)
		[[ $# -ge 2 ]] || fail "--kernel-elf requires a path"
		KERNEL_ELF=$2
		shift 2
		;;
	--ramdisk)
		[[ $# -ge 2 ]] || fail "--ramdisk requires a path"
		RAMDISK=$2
		shift 2
		;;
	--mkbootimg)
		[[ $# -ge 2 ]] || fail "--mkbootimg requires a path"
		MKBOOTIMG=$2
		shift 2
		;;
	--output)
		[[ $# -ge 2 ]] || fail "--output requires a path"
		OUTPUT=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[[ -n "$TARGET_FILES" ]] || fail "--target-files is required"
[[ -n "$KERNEL_ELF" ]] || fail "--kernel-elf is required"
[[ -n "$RAMDISK" ]] || fail "--ramdisk is required"
[[ -n "$MKBOOTIMG" ]] || fail "--mkbootimg is required"
[[ -n "$OUTPUT" ]] || fail "--output is required"
absolute_path "target-files archive" "$TARGET_FILES"
absolute_path "kernel ELF" "$KERNEL_ELF"
absolute_path "normal ramdisk" "$RAMDISK"
absolute_path "mkbootimg" "$MKBOOTIMG"
absolute_path "output disk" "$OUTPUT"
[[ "$(uname -s)" == Linux ]] || fail "builder host must be Linux"
[[ "$(uname -m)" == x86_64 ]] || fail "builder host must be x86_64"
[[ -x "$TARGET_FILES_VERIFIER" ]] || fail "target-files verifier is missing"
[[ -x "$DISK_TOOL" ]] || fail "UBD disk tool is missing"
[[ -x "$BOOT_IMAGE_TOOL" ]] || fail "boot-image verifier is missing"
[[ -x "$INITRAMFS_VERIFIER" ]] || fail "initramfs verifier is missing"
[[ -f "$LAYOUT" ]] || fail "fixed UBD partition layout is missing"
[[ -f "$TARGET_FILES" && ! -L "$TARGET_FILES" ]] ||
	fail "target-files archive must be a regular non-symlink file"
[[ -f "$KERNEL_ELF" && ! -L "$KERNEL_ELF" ]] ||
	fail "kernel ELF must be a regular non-symlink file"
[[ -f "$RAMDISK" && ! -L "$RAMDISK" ]] ||
	fail "normal ramdisk must be a regular non-symlink file"
[[ -f "$MKBOOTIMG" && -x "$MKBOOTIMG" ]] || fail "mkbootimg is not executable"
case "$TARGET_FILES" in
*.zip) ;;
*) fail "target-files archive must end in .zip" ;;
esac
TARGET_FILES_DIR=${TARGET_FILES%.zip}
IMAGES_DIR="$TARGET_FILES_DIR/IMAGES"
[[ -d "$IMAGES_DIR" && ! -L "$IMAGES_DIR" ]] ||
	fail "target-files IMAGES staging directory is missing"
RECOVERY_IMAGE="$IMAGES_DIR/recovery.img"
[[ -f "$RECOVERY_IMAGE" && ! -L "$RECOVERY_IMAGE" ]] ||
	fail "target-files AOSP recovery image is missing"

"$TARGET_FILES_VERIFIER" --target-files "$TARGET_FILES" \
	--kernel-elf "$KERNEL_ELF"
"$INITRAMFS_VERIFIER" "$RAMDISK"
WORK_DIR=$(mktemp -d)
INIT_BOOT_IMAGE="$WORK_DIR/init_boot.img"
"$MKBOOTIMG" --header_version 4 --ramdisk "$RAMDISK" --output "$INIT_BOOT_IMAGE"
"$BOOT_IMAGE_TOOL" verify --image "$INIT_BOOT_IMAGE" --ramdisk "$RAMDISK"
"$BOOT_IMAGE_TOOL" verify --image "$RECOVERY_IMAGE"
"$DISK_TOOL" assemble --layout "$LAYOUT" --images-dir "$IMAGES_DIR" \
	--image "init_boot=$INIT_BOOT_IMAGE" \
	--blank-partition metadata --blank-partition misc --output "$OUTPUT"
"$BOOT_IMAGE_TOOL" verify --layout "$LAYOUT" --disk "$OUTPUT" \
	--partition init_boot --ramdisk "$RAMDISK"
"$BOOT_IMAGE_TOOL" verify --layout "$LAYOUT" --disk "$OUTPUT" \
	--partition recovery
printf '%s\n' "assembled Android 17 UBD disk: $OUTPUT"
