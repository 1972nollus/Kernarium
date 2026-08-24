#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Build the fixed AOSP 17 arm64-only page-agnostic UML product.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ANDROID_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
METADATA="$ANDROID_ROOT/metadata/aosp.env"
VERIFIER="$ANDROID_ROOT/tools/verify-aosp-patches.py"
JELLY_VERIFIER="$ANDROID_ROOT/tools/verify-jelly-source.py"
TARGET_FILES_VERIFIER="$ANDROID_ROOT/tools/verify-target-files.py"
ELF_VERIFIER="$SCRIPT_DIR/verify-kernel-elf.py"
PARTITION_LAYOUT="$ANDROID_ROOT/support/image/partition-layout.json"

AOSP_SOURCE=
OUT_DIR=
KERNEL_ELF=
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
       --out ABSOLUTE_PATH \
       --kernel-elf ABSOLUTE_PATH [--jobs N]

Run locally on the x86_64 Linux builder after applying the routed AOSP patch
series. The output directory must be an ignored directory inside the AOSP Repo
workspace; the pinned Android 17 Soong tree rejects generated host-tool paths
from an external absolute OUT_DIR. Reusing the same output path lets the
Android build system select and preserve its compatible incremental state; a
missing output path is initialized automatically.
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

partition_size_from_layout()
{
	python3 -B - "$PARTITION_LAYOUT" "$1" <<'PY'
import json
import sys

layout_path, partition_name = sys.argv[1:]
with open(layout_path, encoding="utf-8") as source:
    layout = json.load(source)
matches = [
    partition for partition in layout.get("partitions", [])
    if partition.get("name") == partition_name
]
if len(matches) != 1 or not isinstance(matches[0].get("size_bytes"), int):
    raise SystemExit(f"invalid partition size in {layout_path}: {partition_name}")
print(matches[0]["size_bytes"])
PY
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
	--kernel-elf)
		[[ $# -ge 2 ]] || fail "--kernel-elf requires a path"
		KERNEL_ELF=$2
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
[[ -n "$KERNEL_ELF" ]] || fail "--kernel-elf is required"
absolute_path "AOSP source" "$AOSP_SOURCE"
absolute_path "output directory" "$OUT_DIR"
absolute_path "kernel ELF" "$KERNEL_ELF"
[[ "$(uname -s)" == Linux ]] || fail "builder host must be Linux"
[[ "$(uname -m)" == x86_64 ]] || fail "builder host must be x86_64"
command -v zip >/dev/null 2>&1 || fail "zip is not available on PATH"
command -v python3 >/dev/null 2>&1 || fail "python3 is not available on PATH"
[[ -d "$AOSP_SOURCE/.repo/manifests" ]] || fail "AOSP Repo metadata is missing"
[[ -f "$KERNEL_ELF" ]] || fail "kernel ELF does not exist: $KERNEL_ELF"
KERNEL_BUILD_DIR=$(dirname -- "$KERNEL_ELF")
KERNEL_CONFIG="$KERNEL_BUILD_DIR/.config"
KERNEL_RELEASE_FILE="$KERNEL_BUILD_DIR/include/config/kernel.release"
[[ -f "$KERNEL_CONFIG" ]] || fail "kernel configuration is missing: $KERNEL_CONFIG"
[[ -f "$KERNEL_RELEASE_FILE" ]] || fail "kernel release file is missing: $KERNEL_RELEASE_FILE"
[[ -f "$PARTITION_LAYOUT" ]] || fail "fixed UBD partition layout is missing"
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
[[ -x "$JELLY_VERIFIER" ]] || fail "Jelly source verifier is missing"
[[ -x "$TARGET_FILES_VERIFIER" ]] || fail "target-files verifier is missing"
[[ -x "$ELF_VERIFIER" ]] || fail "kernel ELF verifier is missing"
# shellcheck disable=SC1090
source "$METADATA"

"$JELLY_VERIFIER" --android-root "$ANDROID_ROOT" \
	--aosp-source "$AOSP_SOURCE"
"$VERIFIER" --android-root "$ANDROID_ROOT" \
	--aosp-source "$AOSP_SOURCE" --expect final
"$ELF_VERIFIER" "$KERNEL_ELF"
SOONG_BUILD_GRAPH="$OUT_DIR/soong/build.${ANDROID17_PRODUCT}.ninja"
mkdir -p -- "$OUT_DIR"
if [[ -f "$SOONG_BUILD_GRAPH" ]]; then
	printf '%s\n' "reusing incremental AOSP output: $OUT_DIR"
else
	printf '%s\n' "bootstrapping one-time AOSP output: $OUT_DIR"
fi
OUT_DIR_ABSOLUTE=$OUT_DIR
# This tag's test_package path validation treats absolute generated paths as
# module source paths. The verified suffix names the same in-tree directory
# while keeping Soong's generated paths relative to the Repo root.
OUT_DIR=${OUT_DIR#"$AOSP_SOURCE"/}
export OUT_DIR
export SOONG_NINJA=ninja
# Do not force a Soong analysis mode here. The pinned build environment owns
# that choice, and changing modes on a populated OUT_DIR invalidates otherwise
# reusable intermediates.

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
	PRODUCT_USE_DYNAMIC_PARTITIONS \
	PRODUCT_USE_DYNAMIC_PARTITION_SIZE \
	PRODUCT_BUILD_SUPER_PARTITION \
	PRODUCT_BUILD_SUPER_EMPTY_IMAGE \
	PRODUCT_BUILD_SYSTEM_OTHER_IMAGE \
	PRODUCT_VIRTUAL_AB_OTA \
	PRODUCT_VIRTUAL_AB_COMPRESSION \
	PRODUCT_OTA_FORCE_NON_AB_PACKAGE \
	AB_OTA_UPDATER \
	TARGET_OTA_ALLOW_NON_AB \
	BOARD_AVB_ENABLE; do
	[[ "$(get_build_var "$variable")" == false ]] ||
		fail "$variable must be false for the fixed UBD product"
done
for variable in \
	PRODUCT_VIRTUAL_AB_COMPRESSION_METHOD \
	PRODUCT_VIRTUAL_AB_COMPRESSION_FACTOR \
	BOARD_SUPER_PARTITION_SIZE \
	BOARD_SUPER_PARTITION_GROUPS \
	AB_OTA_PARTITIONS \
	AB_OTA_POSTINSTALL_CONFIG; do
	[[ -z "$(get_build_var "$variable")" ]] ||
		fail "$variable must be empty for the fixed UBD product"
done
while read -r variable partition; do
	expected_size=$(partition_size_from_layout "$partition")
	[[ "$(get_build_var "$variable")" == "$expected_size" ]] ||
		fail "$variable does not match the fixed UBD layout"
done <<'EOF'
BOARD_SYSTEMIMAGE_PARTITION_SIZE system
BOARD_SYSTEM_EXTIMAGE_PARTITION_SIZE system_ext
BOARD_PRODUCTIMAGE_PARTITION_SIZE product
BOARD_VENDORIMAGE_PARTITION_SIZE vendor
BOARD_ODMIMAGE_PARTITION_SIZE odm
BOARD_METADATAIMAGE_PARTITION_SIZE metadata
BOARD_USERDATAIMAGE_PARTITION_SIZE userdata
BOARD_INIT_BOOT_IMAGE_PARTITION_SIZE init_boot
BOARD_RECOVERYIMAGE_PARTITION_SIZE recovery
EOF
[[ "$(get_build_var TARGET_NO_RECOVERY)" == false ]] ||
	fail "TARGET_NO_RECOVERY must be false for the recovery partition"
[[ "$(get_build_var PRODUCT_BUILD_RECOVERY_IMAGE)" == true ]] ||
	fail "PRODUCT_BUILD_RECOVERY_IMAGE must be true"
[[ "$(get_build_var BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE)" == true ]] ||
	fail "recovery must use the separately built ACK kernel"
[[ "$(get_build_var BOARD_USES_FULL_RECOVERY_IMAGE)" == true ]] ||
	fail "recovery must remain a full standalone partition image"
[[ "$(get_build_var BOARD_BOOT_HEADER_VERSION)" == 4 ]] ||
	fail "recovery must use boot image header v4"
[[ "$(get_build_var BOARD_INIT_BOOT_HEADER_VERSION)" == 4 ]] ||
	fail "init_boot must use boot image header v4"
for property in $(get_build_var PRODUCT_VENDOR_PROPERTIES); do
	case "$property" in
	ro.virtual_ab.*) fail "Virtual A/B property remains enabled: $property" ;;
	external_storage.projid.enabled=*)
		fail "project-quota property remains enabled: $property"
		;;
	esac
done
[[ -z "$(get_build_var PRODUCT_QUOTA_PROJID)" ]] ||
	fail "PRODUCT_QUOTA_PROJID must be empty for the UML userdata filesystem"
for variable in \
	PRODUCT_BUILD_SYSTEM_DLKM_IMAGE \
	PRODUCT_BUILD_VENDOR_DLKM_IMAGE \
	PRODUCT_BUILD_ODM_DLKM_IMAGE; do
	[[ "$(get_build_var "$variable")" == false ]] ||
		fail "$variable must be false for the built-in-only kernel"
done
for variable in \
	BOARD_USES_SYSTEM_DLKMIMAGE \
	BOARD_USES_VENDOR_DLKMIMAGE \
	BOARD_USES_ODM_DLKMIMAGE \
	BOARD_SYSTEM_DLKMIMAGE_FILE_SYSTEM_TYPE \
	BOARD_VENDOR_DLKMIMAGE_FILE_SYSTEM_TYPE \
	BOARD_ODM_DLKMIMAGE_FILE_SYSTEM_TYPE; do
	[[ -z "$(get_build_var "$variable")" ]] ||
		fail "$variable must be empty for the built-in-only kernel"
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

[[ "$(get_build_var TRUSTY_SYSTEM_VM)" == disabled ]] ||
	fail "TRUSTY_SYSTEM_VM must be disabled for the UML guest"
PRODUCT_PACKAGES=" $(get_build_var PRODUCT_PACKAGES) "
for package in \
	com.android.hardware.gatekeeper.nonsecure \
	com.android.hardware.keymint.rust_nonsecure \
	Jelly \
	LatinIME \
	ThemePicker; do
	[[ "$PRODUCT_PACKAGES" == *" $package "* ]] ||
		fail "required UML product package is absent: $package"
done
for package in \
	BasicDreams \
	BluetoothMidiService \
	Calendar \
	Camera2 \
	Contacts \
	DeskClock \
	Dialer \
	Gallery2 \
	TeleService \
	Telecom \
	TelephonyProvider \
	com.android.hardware.wifi \
	com.google.cf.wifi \
	com.google.cf.wpa_supplicant \
	messaging; do
	[[ "$PRODUCT_PACKAGES" != *" $package "* ]] ||
		fail "unneeded built-in application remains present: $package"
done
for package in \
	com.android.hardware.gatekeeper.cf_remote \
	com.android.hardware.keymint.rust_cf_guest_trusty_nonsecure \
	com.android.hardware.keymint.rust_cf_remote \
	trusty_security_vm.elf \
	trusty_security_vm_launcher \
	trusty_security_vm_launcher.rc; do
	[[ "$PRODUCT_PACKAGES" != *" $package "* ]] ||
		fail "unsupported Trusty or remote security package is present: $package"
done
[[ "$PRODUCT_PACKAGES" != *" dlkm_loader "* ]] ||
	fail "dlkm_loader is forbidden for the built-in-only kernel"
for package in \
	android.hardware.boot-service.default_recovery \
	checkpoint_gc \
	com.android.hardware.boot \
	update_engine_sideload; do
	[[ "$PRODUCT_PACKAGES" != *" $package "* ]] ||
		fail "Cuttlefish A/B package is forbidden for the fixed UBD product: $package"
done
for package in \
	ThreadNetworkDemoApp \
	android.hardware.sensors-service.multihal \
	android.hardware.sensors@2.1-impl.cuttlefish \
	com.android.hardware.neuralnetworks \
	com.android.hardware.threadnetwork \
	com.android.hardware.uwb \
	com.google.cf.bt \
	com.google.cf.light \
	com.google.cf.oemlock \
	cuttlefish_overlay_uwb \
	cuttlefish_overlay_uwb_gsi \
	cuttlefish_sensor_injection; do
	[[ "$PRODUCT_PACKAGES" != *" $package "* ]] ||
		fail "unsupported Cuttlefish HAL package is present: $package"
done

# target-files always carries a kernel input. Stage only the already-built,
# verified ACK UML ELF in the output tree; the product's init_boot and recovery
# images deliberately exclude it, and no AOSP source project owns a copy.
PRODUCT_OUT=$(get_build_var PRODUCT_OUT)
case "$PRODUCT_OUT" in
"$OUT_DIR"/*) ;;
*) fail "PRODUCT_OUT is outside the selected output directory: $PRODUCT_OUT" ;;
esac
mkdir -p -- "$PRODUCT_OUT"
if ! cmp -s -- "$KERNEL_ELF" "$PRODUCT_OUT/kernel"; then
	install -m 0755 -- "$KERNEL_ELF" "$PRODUCT_OUT/kernel"
fi

m -j"$JOBS" target-files-package check_target_files_vintf
TARGET_FILES="$PRODUCT_OUT/obj/PACKAGING/target_files_intermediates/${ANDROID17_PRODUCT}-target_files.zip"
[[ -f "$TARGET_FILES" ]] || fail "target-files archive is missing: $TARGET_FILES"
"$TARGET_FILES_VERIFIER" --target-files "$TARGET_FILES" \
	--kernel-elf "$KERNEL_ELF" \
	--kernel-config "$KERNEL_CONFIG" \
	--kernel-release-file "$KERNEL_RELEASE_FILE"
HOST_BIN="$OUT_DIR_ABSOLUTE/host/linux-x86/bin"
VINTF_CHECKER="$HOST_BIN/check_target_files_vintf"
[[ -x "$VINTF_CHECKER" ]] || fail "packaged VINTF checker is missing: $VINTF_CHECKER"
PATH="$HOST_BIN:/usr/bin:/bin" "$VINTF_CHECKER" "$TARGET_FILES"
"$VERIFIER" --android-root "$ANDROID_ROOT" \
	--aosp-source "$AOSP_SOURCE" --expect final
printf '%s\n' "built AOSP 17 UML target files under: $OUT_DIR_ABSOLUTE"
