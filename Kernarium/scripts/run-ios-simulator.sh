#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build, install, provision, and launch Kernarium on a booted Simulator.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ROOTFS="$ROOT_DIR/rootfs.erofs"
KERNEL_IMAGE=${UML_KERNARIUM_KERNEL_IMAGE:-/Volumes/Kernel/build-16k/linux}
DEVICE=${KERNARIUM_SIMULATOR_UDID:-booted}
BUNDLE_ID=com.imxieyi.Kernarium
DERIVED_DATA="$ROOT_DIR/.build/kernarium-xcode-derived"

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
		"  --kernel PATH  Maintained 16 KiB UML kernel image" \
		"  --device UDID  Booted Simulator UDID (default: booted)" \
		"  -h, --help      Show this help"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--rootfs)
		[ "$#" -ge 2 ] || fail "--rootfs requires a path"
		ROOTFS=$2
		shift 2
		;;
	--kernel)
		[ "$#" -ge 2 ] || fail "--kernel requires a path"
		KERNEL_IMAGE=$2
		shift 2
		;;
	--device)
		[ "$#" -ge 2 ] || fail "--device requires a UDID"
		DEVICE=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -f "$ROOTFS" ] || fail "rootfs image is unavailable: $ROOTFS"
[ -f "$KERNEL_IMAGE" ] || fail "kernel image is unavailable: $KERNEL_IMAGE"
if [ "$DEVICE" = booted ]; then
	DEVICE=$(xcrun simctl getenv booted SIMULATOR_UDID)
fi
[ -n "$DEVICE" ] || fail "no booted Simulator is available"

cd "$ROOT_DIR"
cmake --preset kernarium-ios-simulator \
	-DUML_KERNARIUM_KERNEL_IMAGE="$KERNEL_IMAGE"
cmake --build --preset kernarium-ios-simulator
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
exec xcrun simctl launch --console-pty "$DEVICE" "$BUNDLE_ID"
