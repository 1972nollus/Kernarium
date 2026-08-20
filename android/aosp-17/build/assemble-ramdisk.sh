#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Compose the generic and UML first-stage Android ramdisks into raw newc.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ANDROID_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
TARGET_FILES_VERIFIER="$ANDROID_ROOT/tools/verify-target-files.py"
INITRAMFS_VERIFIER="$ANDROID_ROOT/tools/verify-initramfs.py"

TARGET_FILES=
KERNEL_ELF=
MKBOOTFS=
OUTPUT=
WORK_DIR=
TEMP_OUTPUT=

fail()
{
	printf '%s\n' "android/aosp-17/build/assemble-ramdisk.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: android/aosp-17/build/assemble-ramdisk.sh \
       --target-files ABSOLUTE_PATH \
       --kernel-elf ABSOLUTE_PATH \
       --mkbootfs ABSOLUTE_PATH \
       --output ABSOLUTE_PATH

Run locally on the x86_64 Linux builder after build-aosp.sh. The target-files
staging directory must remain beside its .zip archive. The output is a raw,
uncompressed newc initramfs for the external UML initrd path; an existing path
is never replaced.
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
	if [[ -n "$TEMP_OUTPUT" && -f "$TEMP_OUTPUT" ]]; then
		rm -f -- "$TEMP_OUTPUT"
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
	--mkbootfs)
		[[ $# -ge 2 ]] || fail "--mkbootfs requires a path"
		MKBOOTFS=$2
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
[[ -n "$MKBOOTFS" ]] || fail "--mkbootfs is required"
[[ -n "$OUTPUT" ]] || fail "--output is required"
absolute_path "target-files archive" "$TARGET_FILES"
absolute_path "kernel ELF" "$KERNEL_ELF"
absolute_path "mkbootfs" "$MKBOOTFS"
absolute_path "output initramfs" "$OUTPUT"
[[ "$(uname -s)" == Linux ]] || fail "builder host must be Linux"
[[ "$(uname -m)" == x86_64 ]] || fail "builder host must be x86_64"
[[ -x "$TARGET_FILES_VERIFIER" ]] || fail "target-files verifier is missing"
[[ -x "$INITRAMFS_VERIFIER" ]] || fail "initramfs verifier is missing"
[[ -f "$TARGET_FILES" && ! -L "$TARGET_FILES" ]] ||
	fail "target-files archive must be a regular non-symlink file"
[[ -f "$KERNEL_ELF" && ! -L "$KERNEL_ELF" ]] ||
	fail "kernel ELF must be a regular non-symlink file"
[[ -f "$MKBOOTFS" && -x "$MKBOOTFS" ]] || fail "mkbootfs is not executable"
[[ ! -e "$OUTPUT" && ! -L "$OUTPUT" ]] || fail "output already exists"
OUTPUT_DIR=$(dirname -- "$OUTPUT")
[[ -d "$OUTPUT_DIR" && ! -L "$OUTPUT_DIR" ]] ||
	fail "output parent must be an existing non-symlink directory"
case "$TARGET_FILES" in
*.zip) ;;
*) fail "target-files archive must end in .zip" ;;
esac

TARGET_FILES_DIR=${TARGET_FILES%.zip}
RAMDISK_SOURCE="$TARGET_FILES_DIR/INIT_BOOT/RAMDISK"
VENDOR_FSTAB="$TARGET_FILES_DIR/VENDOR_BOOT/RAMDISK/first_stage_ramdisk/system/etc/fstab.uml"
FILESYSTEM_CONFIG_SOURCE="$TARGET_FILES_DIR/META/init_boot_filesystem_config.txt"
NODE_LIST="$TARGET_FILES_DIR/META/ramdisk_node_list"
for path in "$RAMDISK_SOURCE" "$FILESYSTEM_CONFIG_SOURCE" "$NODE_LIST" "$VENDOR_FSTAB"; do
	[[ -e "$path" && ! -L "$path" ]] || fail "required staging input is missing: $path"
done

"$TARGET_FILES_VERIFIER" --target-files "$TARGET_FILES" \
	--kernel-elf "$KERNEL_ELF"

WORK_DIR=$(mktemp -d)
RAMDISK_TREE="$WORK_DIR/ramdisk"
FILESYSTEM_CONFIG="$WORK_DIR/filesystem-config.txt"
mkdir -p -- "$RAMDISK_TREE"
cp -a -- "$RAMDISK_SOURCE/." "$RAMDISK_TREE/"
for path in \
	system/bin/modprobe \
	system/bin/snapuserd \
	system/bin/snapuserd_ramdisk \
	system/etc/init/snapuserd.rc; do
	[[ ! -d "$RAMDISK_TREE/$path" ]] || fail "refusing to remove directory: $path"
	rm -f -- "$RAMDISK_TREE/$path"
done

install -d -m 0755 -- "$RAMDISK_TREE/first_stage_ramdisk/system/etc"
install -m 0644 -- "$VENDOR_FSTAB" \
	"$RAMDISK_TREE/first_stage_ramdisk/system/etc/fstab.uml"
if grep -Eq '^first_stage_ramdisk/system(/| )' "$FILESYSTEM_CONFIG_SOURCE"; then
	fail "generic ramdisk already owns first_stage_ramdisk/system"
fi
grep -Ev '^(system/bin/(modprobe|snapuserd|snapuserd_ramdisk)|system/etc/init/snapuserd\.rc) ' \
	"$FILESYSTEM_CONFIG_SOURCE" > "$FILESYSTEM_CONFIG"
cat >> "$FILESYSTEM_CONFIG" <<'EOF'
first_stage_ramdisk/system 0 0 755 capabilities=0x0
first_stage_ramdisk/system/etc 0 0 755 capabilities=0x0
first_stage_ramdisk/system/etc/fstab.uml 0 0 644 capabilities=0x0
EOF

TEMP_OUTPUT=$(mktemp "$OUTPUT_DIR/.android17-ramdisk.XXXXXX")
"$MKBOOTFS" -f "$FILESYSTEM_CONFIG" -n "$NODE_LIST" "$RAMDISK_TREE" > "$TEMP_OUTPUT"
"$INITRAMFS_VERIFIER" "$TEMP_OUTPUT"
chmod 0644 -- "$TEMP_OUTPUT"
mv -- "$TEMP_OUTPUT" "$OUTPUT"
TEMP_OUTPUT=
printf '%s\n' "assembled Android 17 UML initramfs: $OUTPUT"
