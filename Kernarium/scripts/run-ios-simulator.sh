#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build, install, provision, and launch Kernarium on a booted Simulator.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ROOTFS="$ROOT_DIR/rootfs.erofs"
# shellcheck disable=SC1091
. "$ROOT_DIR/kernel/mainline-6.18/artifacts/accepted-16k.env"
ARTIFACT_16K=${UML_MAINLINE_618_KERNEL_ARTIFACT_16K_DIR:-"$ROOT_DIR/.build/kernel/artifacts/$UML_KERNEL_ARTIFACT_DIRECTORY_NAME"}
# shellcheck disable=SC1091
. "$ROOT_DIR/kernel/mainline-6.18/artifacts/accepted-4k.env"
ARTIFACT_4K=${UML_MAINLINE_618_KERNEL_ARTIFACT_4K_DIR:-"$ROOT_DIR/.build/kernel/artifacts/$UML_KERNEL_ARTIFACT_DIRECTORY_NAME"}
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
		"  --rootfs PATH  EROFS or sealed ext4 image for app Documents" \
		"  --artifact-16k PATH  Accepted 16 KiB kernel artifact directory" \
		"  --artifact-4k PATH   Accepted 4 KiB kernel artifact directory" \
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
	--artifact-16k)
		[ "$#" -ge 2 ] || fail "--artifact-16k requires a path"
		ARTIFACT_16K=$2
		shift 2
		;;
	--artifact-4k)
		[ "$#" -ge 2 ] || fail "--artifact-4k requires a path"
		ARTIFACT_4K=$2
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
case "$ROOTFS" in
*.erofs) ROOTFS_FILENAME=rootfs.erofs ;;
*.ext4) ROOTFS_FILENAME=rootfs.ext4 ;;
*) fail "rootfs must have an .erofs or .ext4 suffix: $ROOTFS" ;;
esac
[ -d "$ARTIFACT_16K" ] ||
	fail "16 KiB kernel artifact is unavailable: $ARTIFACT_16K"
[ -d "$ARTIFACT_4K" ] ||
	fail "4 KiB kernel artifact is unavailable: $ARTIFACT_4K"
if [ "$DEVICE" = booted ]; then
	DEVICE=$(xcrun simctl getenv booted SIMULATOR_UDID)
fi
[ -n "$DEVICE" ] || fail "no booted Simulator is available"

cd "$ROOT_DIR"
cmake --preset kernarium-ios-simulator \
	-DUML_MAINLINE_618_KERNEL_ARTIFACT_DIR="$ARTIFACT_16K"
cmake --build --preset kernarium-ios-simulator
cmake --preset kernarium-ios-simulator-4k \
	-DUML_MAINLINE_618_KERNEL_ARTIFACT_DIR="$ARTIFACT_4K"
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
rm -f -- "$DATA_CONTAINER/Documents/$ROOTFS_FILENAME"
cp "$ROOTFS" "$DATA_CONTAINER/Documents/$ROOTFS_FILENAME"
if [ "$ROOTFS_FILENAME" = rootfs.ext4 ]; then
	chmod 0444 "$DATA_CONTAINER/Documents/$ROOTFS_FILENAME"
fi
export SIMCTL_CHILD_KERNARIUM_ROOTFS=$ROOTFS_FILENAME
printf '%s\n' \
	"copied rootfs to $DATA_CONTAINER/Documents/$ROOTFS_FILENAME" \
	"launching Kernarium; guest console follows on stdout"
if [ "$AQUARIUM" -eq 1 ]; then
	export SIMCTL_CHILD_KERNARIUM_WEBGL_AQUARIUM=1
	export SIMCTL_CHILD_KERNARIUM_EXECUTION_ENGINE=gadgets
	export SIMCTL_CHILD_KERNARIUM_VCPU_COUNT=4
	export SIMCTL_CHILD_KERNARIUM_GUEST_MEMORY_MIB=4096
	export SIMCTL_CHILD_KERNARIUM_OVERLAY_SIZE_GIB=8
	export SIMCTL_CHILD_KERNARIUM_RETINA_MODE=0
	export SIMCTL_CHILD_UML_GPU_FRAME_STATS=1
fi
exec xcrun simctl launch --console-pty "$DEVICE" "$BUNDLE_ID"
