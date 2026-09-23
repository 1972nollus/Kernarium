#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build, install, provision, and launch the Kernarium iOS app on this Apple
# silicon Mac in Designed for iPad mode.
#
# The iOS Simulator cannot host this stack: Mesa Zink renders on KosmicKrisp,
# which requires Metal 4, and the Simulator exposes no Metal 4 device. The same
# unmodified iOS device slice runs natively on an Apple silicon Mac, where the
# host GPU is used directly, so that mode is the iOS verification vehicle until
# a physical device is attached.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ROOTFS="$ROOT_DIR/rootfs.erofs"
# The device runtimes render through the Vulkan-experiment GPU stack, whose
# guest reaches host-visible blob memory through the vhost-user shared-memory
# aperture that only the Vulkan-experiment kernel line carries. The presets
# select that product; stage the artifacts it names.
# shellcheck disable=SC1091
. "$ROOT_DIR/kernel/mainline-6.18/artifacts/vulkan-experiment-16k.env"
ARTIFACT_16K=${UML_MAINLINE_618_KERNEL_ARTIFACT_16K_DIR:-"$ROOT_DIR/.build/kernel/artifacts/$UML_KERNEL_ARTIFACT_DIRECTORY_NAME"}
# shellcheck disable=SC1091
. "$ROOT_DIR/kernel/mainline-6.18/artifacts/vulkan-experiment-4k.env"
ARTIFACT_4K=${UML_MAINLINE_618_KERNEL_ARTIFACT_4K_DIR:-"$ROOT_DIR/.build/kernel/artifacts/$UML_KERNEL_ARTIFACT_DIRECTORY_NAME"}
BUNDLE_ID=com.imxieyi.Kernarium
DERIVED_DATA="$ROOT_DIR/.build/kernarium-xcode-derived"
DESTINATION='platform=macOS,variant=Designed for iPad'
AQUARIUM=0
GUEST_BUNDLE=
JOBS=4
SKIP_BUILD=0

fail()
{
	printf '%s\n' "Kernarium/scripts/run-ios-mac.sh: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: Kernarium/scripts/run-ios-mac.sh [options]" \
		"" \
		"Options:" \
		"  --rootfs PATH  EROFS or sealed ext4 image for app Documents" \
		"  --guest-bundle NAME  Assemble NAME.kern from the accepted" \
		"                 Android release and select it; its writable disk" \
		"                 is kept across runs" \
		"  --artifact-16k PATH  Accepted 16 KiB kernel artifact directory" \
		"  --artifact-4k PATH   Accepted 4 KiB kernel artifact directory" \
		"  --jobs NUMBER  Parallel build jobs (default: 4)" \
		"  --no-build     Stage and launch the installed app as it stands" \
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
	--guest-bundle)
		[ "$#" -ge 2 ] || fail "--guest-bundle requires a name"
		GUEST_BUNDLE=$2
		shift 2
		;;
	--jobs)
		[ "$#" -ge 2 ] || fail "--jobs requires a number"
		JOBS=$2
		shift 2
		;;
	--no-build)
		SKIP_BUILD=1
		shift
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

cd "$ROOT_DIR"
if [ "$SKIP_BUILD" -eq 0 ]; then
	cmake --preset kernarium-ios-device \
		-DUML_MAINLINE_618_KERNEL_ARTIFACT_DIR="$ARTIFACT_16K"
	cmake --build --preset kernarium-ios-device --parallel "$JOBS"
	cmake --preset kernarium-ios-device-4k \
		-DUML_MAINLINE_618_KERNEL_ARTIFACT_DIR="$ARTIFACT_4K"
	cmake --build --preset kernarium-ios-device-4k --parallel "$JOBS"
	"$ROOT_DIR/Kernarium/scripts/package-ios-frameworks.sh"
	xcodebuild \
		-project "$ROOT_DIR/Kernarium/Kernarium.xcodeproj" \
		-scheme Kernarium \
		-configuration Release \
		-destination "$DESTINATION" \
		-derivedDataPath "$DERIVED_DATA" \
		-jobs "$JOBS" \
		build
fi

# An iOS application on macOS is sandboxed into a per-bundle data container and
# cannot read a path outside it, so the rootfs is copied in rather than
# referenced in place.
resolve_container()
{
	python3 -B - "$BUNDLE_ID" <<'PYTHON'
import pathlib
import plistlib
import sys

identifier = sys.argv[1]
root = pathlib.Path.home() / "Library" / "Containers"
newest = None
for metadata in root.glob("*/.com.apple.containermanagerd.metadata.plist"):
    try:
        with metadata.open("rb") as handle:
            payload = plistlib.load(handle)
    except (OSError, plistlib.InvalidFileException):
        continue
    if payload.get("MCMMetadataIdentifier") != identifier:
        continue
    data = metadata.parent / "Data"
    if not data.is_dir():
        continue
    stamp = metadata.parent.stat().st_mtime
    if newest is None or stamp > newest[0]:
        newest = (stamp, data)
if newest is None:
    raise SystemExit(1)
print(newest[1])
PYTHON
}

DATA_CONTAINER=$(resolve_container) || DATA_CONTAINER=
if [ -z "$DATA_CONTAINER" ]; then
	[ "$SKIP_BUILD" -eq 0 ] ||
		fail "no data container for $BUNDLE_ID; run without --no-build"
	# macOS creates the per-bundle container when it installs the
	# application, and only Xcode's local install service installs an iOS
	# application here, so provoke one install before staging into it.
	xcodebuild \
		-project "$ROOT_DIR/Kernarium/Kernarium.xcodeproj" \
		-scheme Kernarium \
		-configuration Release \
		-destination "$DESTINATION" \
		-derivedDataPath "$DERIVED_DATA" \
		-jobs "$JOBS" \
		-only-testing:KernariumTests \
		ENABLE_TESTABILITY=YES \
		test
	DATA_CONTAINER=$(resolve_container) ||
		fail "macOS did not create a data container for $BUNDLE_ID"
fi

mkdir -p -- "$DATA_CONTAINER/Documents"
rm -f -- "$DATA_CONTAINER/Documents/$ROOTFS_FILENAME"
cp "$ROOTFS" "$DATA_CONTAINER/Documents/$ROOTFS_FILENAME"
if [ "$ROOTFS_FILENAME" = rootfs.ext4 ]; then
	chmod 0444 "$DATA_CONTAINER/Documents/$ROOTFS_FILENAME"
fi
LAUNCH_ROOTFS=$ROOTFS_FILENAME
printf '%s\n' "copied rootfs to $DATA_CONTAINER/Documents/$ROOTFS_FILENAME"
if [ -n "$GUEST_BUNDLE" ]; then
	case "$GUEST_BUNDLE" in
	*/*|"") fail "--guest-bundle takes a plain name, not a path" ;;
	esac
	BUNDLE_PATH="$DATA_CONTAINER/Documents/${GUEST_BUNDLE%.kern}.kern"
	# Assembling again would discard the writable disk the guest has been
	# accumulating, so an existing bundle is reused as it stands.
	if [ -d "$BUNDLE_PATH" ]; then
		printf '%s\n' "reusing existing guest bundle $BUNDLE_PATH"
	else
		python3 -B "$ROOT_DIR/Kernarium/scripts/make-guest-bundle.py" \
			--output "$BUNDLE_PATH"
	fi
	LAUNCH_ROOTFS=${GUEST_BUNDLE%.kern}.kern
fi

set -- -e "TEST_RUNNER_KERNARIUM_ROOTFS=$LAUNCH_ROOTFS"
if [ "$AQUARIUM" -eq 1 ]; then
	set -- "$@" \
		-e TEST_RUNNER_KERNARIUM_WEBGL_AQUARIUM=1 \
		-e TEST_RUNNER_KERNARIUM_EXECUTION_ENGINE=gadgets \
		-e TEST_RUNNER_KERNARIUM_VCPU_COUNT=4 \
		-e TEST_RUNNER_KERNARIUM_GUEST_MEMORY_MIB=4096 \
		-e TEST_RUNNER_KERNARIUM_OVERLAY_SIZE_GIB=8 \
		-e TEST_RUNNER_KERNARIUM_RETINA_MODE=0 \
		-e TEST_RUNNER_UML_GPU_FRAME_STATS=1
fi

# `open` cannot start an iOS application on macOS and `devicectl` does not
# enumerate the host Mac, so Xcode's local install service is the only
# command-line launcher for this mode and `xcodebuild test` is what drives it.
# The app hosts its own test bundle, so this both launches the application and
# checks the contracts that depend on the mode. The launched product stays
# optimized; testability is raised only so the hosted bundle can import it.
printf '%s\n' "launching Kernarium on this Mac in Designed for iPad mode"
exec xcodebuild \
	-project "$ROOT_DIR/Kernarium/Kernarium.xcodeproj" \
	-scheme Kernarium \
	-configuration Release \
	-destination "$DESTINATION" \
	-derivedDataPath "$DERIVED_DATA" \
	-jobs "$JOBS" \
	-only-testing:KernariumTests \
	ENABLE_TESTABILITY=YES \
	"$@" \
	test
