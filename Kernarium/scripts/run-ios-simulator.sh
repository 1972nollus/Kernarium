#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build, install, provision, and launch Kernarium on a booted Simulator.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ROOTFS="$ROOT_DIR/rootfs.erofs"
KERNEL_IMAGE_16K=${UML_KERNARIUM_KERNEL_IMAGE_16K:-/Volumes/Kernel/build-16k/vmlinux}
KERNEL_IMAGE_4K=${UML_KERNARIUM_KERNEL_IMAGE_4K:-/Volumes/Kernel/build-4k/vmlinux}
DEVICE=${KERNARIUM_SIMULATOR_UDID:-booted}
BUNDLE_ID=com.imxieyi.Kernarium
DERIVED_DATA="$ROOT_DIR/.build/kernarium-xcode-derived"
AQUARIUM=0

fail()
{
	printf '%s\n' "Kernarium/scripts/run-ios-simulator.sh: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: Kernarium/scripts/run-ios-simulator.sh [options]" \
		"" \
		"Options:" \
		"  --rootfs PATH  EROFS image to copy into app Documents" \
		"  --kernel-16k PATH  Maintained 16 KiB UML kernel image" \
		"  --kernel-4k PATH   Maintained 4 KiB UML kernel image" \
		"  --kernel PATH      Alias for --kernel-16k" \
		"  --device UDID  Booted Simulator UDID (default: booted)" \
		"  --aquarium     Launch Chromium WebGL Aquarium verification" \
		"  -h, --help      Show this help"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--rootfs)
		[ "$#" -ge 2 ] || fail "--rootfs requires a path"
		ROOTFS=$2
		shift 2
		;;
	--kernel|--kernel-16k)
		[ "$#" -ge 2 ] || fail "$1 requires a path"
		KERNEL_IMAGE_16K=$2
		shift 2
		;;
	--kernel-4k)
		[ "$#" -ge 2 ] || fail "--kernel-4k requires a path"
		KERNEL_IMAGE_4K=$2
		shift 2
		;;
	--device)
		[ "$#" -ge 2 ] || fail "--device requires a UDID"
		DEVICE=$2
		shift 2
		;;
	--aquarium)
		AQUARIUM=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -f "$ROOTFS" ] || fail "rootfs image is unavailable: $ROOTFS"
[ -f "$KERNEL_IMAGE_16K" ] ||
	fail "16 KiB kernel image is unavailable: $KERNEL_IMAGE_16K"
[ -f "$KERNEL_IMAGE_4K" ] ||
	fail "4 KiB kernel image is unavailable: $KERNEL_IMAGE_4K"
if [ "$DEVICE" = booted ]; then
	DEVICE=$(xcrun simctl getenv booted SIMULATOR_UDID)
fi
[ -n "$DEVICE" ] || fail "no booted Simulator is available"

cd "$ROOT_DIR"
cmake --preset kernarium-ios-simulator \
	-DUML_KERNARIUM_KERNEL_IMAGE="$KERNEL_IMAGE_16K"
cmake --build --preset kernarium-ios-simulator
cmake --preset kernarium-ios-simulator-4k \
	-DUML_KERNARIUM_KERNEL_IMAGE="$KERNEL_IMAGE_4K"
cmake --build --preset kernarium-ios-simulator-4k
"$ROOT_DIR/Kernarium/scripts/package-ios-frameworks.sh"
xcodebuild \
	-project "$ROOT_DIR/Kernarium/Kernarium.xcodeproj" \
	-scheme Kernarium \
	-configuration Debug \
	-sdk iphonesimulator \
	-destination "platform=iOS Simulator,id=$DEVICE" \
	-derivedDataPath "$DERIVED_DATA" \
	build CODE_SIGNING_ALLOWED=NO

APP="$DERIVED_DATA/Build/Products/Debug-iphonesimulator/Kernarium.app"
[ -d "$APP" ] || fail "Xcode did not produce the Simulator app: $APP"
xcrun simctl terminate "$DEVICE" "$BUNDLE_ID" >/dev/null 2>&1 || :
xcrun simctl install "$DEVICE" "$APP"
DATA_CONTAINER=$(xcrun simctl get_app_container "$DEVICE" "$BUNDLE_ID" data)
[ -d "$DATA_CONTAINER" ] || fail "cannot locate the app data container"
mkdir -p -- "$DATA_CONTAINER/Documents"
cp "$ROOTFS" "$DATA_CONTAINER/Documents/rootfs.erofs"
printf '%s\n' \
	"copied rootfs to $DATA_CONTAINER/Documents/rootfs.erofs" \
	"launching Kernarium; guest console follows on stdout"
if [ "$AQUARIUM" -eq 1 ]; then
	export SIMCTL_CHILD_KERNARIUM_WEBGL_AQUARIUM=1
	export SIMCTL_CHILD_KERNARIUM_EXECUTION_ENGINE=gadgets
	export SIMCTL_CHILD_KERNARIUM_VCPU_COUNT=4
	export SIMCTL_CHILD_KERNARIUM_GUEST_MEMORY_MIB=1536
	export SIMCTL_CHILD_KERNARIUM_OVERLAY_SIZE_GIB=8
	export SIMCTL_CHILD_KERNARIUM_RETINA_MODE=0
	export SIMCTL_CHILD_UML_GPU_FRAME_STATS=1
fi
exec xcrun simctl launch --console-pty "$DEVICE" "$BUNDLE_ID"
