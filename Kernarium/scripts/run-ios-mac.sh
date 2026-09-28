#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build, install, provision, and launch the Kernarium iOS app on this Apple
# silicon Mac in Designed for iPad mode.
#
# The iOS Simulator cannot host this stack: Mesa Zink renders on KosmicKrisp,
# which requires Metal 4, and the Simulator exposes no Metal 4 device. The same
# unmodified iOS device slice runs natively on an Apple silicon Mac, where the
# host GPU is used directly; the physical-device lanes are in docs/testing.md.
#
# --aquarium builds and packages the automation runtimes instead of the release
# ones and boots the WebGL Aquarium probe as a host payload
# (Kernarium/README.md).
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ROOTFS=
# The device runtime presets embed the accepted mainline kernels; stage the
# artifacts they name.
# shellcheck disable=SC1091
. "$ROOT_DIR/kernel/mainline-6.18/artifacts/accepted-16k.env"
ARTIFACT_16K=${UML_MAINLINE_618_KERNEL_ARTIFACT_16K_DIR:-"$ROOT_DIR/.build/kernel/artifacts/$UML_KERNEL_ARTIFACT_DIRECTORY_NAME"}
# shellcheck disable=SC1091
. "$ROOT_DIR/kernel/mainline-6.18/artifacts/accepted-4k.env"
ARTIFACT_4K=${UML_MAINLINE_618_KERNEL_ARTIFACT_4K_DIR:-"$ROOT_DIR/.build/kernel/artifacts/$UML_KERNEL_ARTIFACT_DIRECTORY_NAME"}
BUNDLE_ID=com.imxieyi.Kernarium
DERIVED_DATA="$ROOT_DIR/.build/kernarium-xcode-derived"
INITRAMFS_TOOL="$ROOT_DIR/.build/presets/macos-integration/bin/uml-initramfs"
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
		"  --rootfs PATH  EROFS or sealed ext4 image to copy into app" \
		"                 Documents under its own name and select" \
		"                 (default: the accepted KDE desktop image that" \
		"                 Kernarium selects when its settings name none)" \
		"  --guest-bundle NAME  Assemble NAME.kern from the accepted" \
		"                 Android release and select it; its writable disk" \
		"                 is kept across runs" \
		"  --artifact-16k PATH  Accepted 16 KiB kernel artifact directory" \
		"  --artifact-4k PATH   Accepted 4 KiB kernel artifact directory" \
		"  --jobs NUMBER  Parallel build jobs (default: 4)" \
		"  --no-build     Stage and launch the installed app as it stands" \
		"  --aquarium     Build the automation runtimes and launch Chromium" \
		"                 WebGL Aquarium verification" \
		"  --initramfs-tool PATH  uml-initramfs, which writes the Aquarium" \
		"                 payload (default: the macos-integration build)" \
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
	--initramfs-tool)
		[ "$#" -ge 2 ] || fail "--initramfs-tool requires a path"
		INITRAMFS_TOOL=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

if [ -z "$ROOTFS" ]; then
	# Kernarium's default image (RootfsImage.defaultFilename) is the accepted
	# KDE release that tools/rootfs/desktop/images.json records.
	ROOTFS=$(python3 -B - "$ROOT_DIR/tools/rootfs/desktop/images.json" <<'PYTHON'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    accepted = json.load(handle)["images"]["kde"]["accepted"]
print(f"{accepted['release']}/{accepted['file']}")
PYTHON
) || fail "cannot read the accepted KDE image from images.json"
	ROOTFS="$ROOT_DIR/.build/rootfs/$ROOTFS"
fi
[ -f "$ROOTFS" ] || fail "rootfs image is unavailable: $ROOTFS"
# The image keeps its name in Documents, as the application's picker shows it,
# and that name reaches a UBD kernel argument (RootfsImage.validatedFilename).
ROOTFS_FILENAME=$(basename -- "$ROOTFS")
case "$ROOTFS_FILENAME" in
*[[:space:],:\"]*) fail "the image name cannot be a kernel argument: $ROOTFS_FILENAME" ;;
*.erofs|*.ext4) ;;
*) fail "rootfs must have an .erofs or .ext4 suffix: $ROOTFS" ;;
esac
[ -d "$ARTIFACT_16K" ] ||
	fail "16 KiB kernel artifact is unavailable: $ARTIFACT_16K"
[ -d "$ARTIFACT_4K" ] ||
	fail "4 KiB kernel artifact is unavailable: $ARTIFACT_4K"
if [ "$AQUARIUM" -eq 1 ]; then
	[ -x "$INITRAMFS_TOOL" ] ||
		fail "uml-initramfs is unavailable: $INITRAMFS_TOOL; build it or pass --initramfs-tool"
	PRESET=kernarium-ios-device-automation
	PACKAGE_OPTION=--automation
else
	PRESET=kernarium-ios-device
	PACKAGE_OPTION=
fi

cd "$ROOT_DIR"
if [ "$SKIP_BUILD" -eq 0 ]; then
	cmake --preset "$PRESET" \
		-DUML_MAINLINE_618_KERNEL_ARTIFACT_DIR="$ARTIFACT_16K"
	cmake --build --preset "$PRESET" --parallel "$JOBS"
	cmake --preset "$PRESET-4k" \
		-DUML_MAINLINE_618_KERNEL_ARTIFACT_DIR="$ARTIFACT_4K"
	cmake --build --preset "$PRESET-4k" --parallel "$JOBS"
	"$ROOT_DIR/Kernarium/scripts/package-ios-frameworks.sh" $PACKAGE_OPTION
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
case "$ROOTFS_FILENAME" in
*.ext4) chmod 0444 "$DATA_CONTAINER/Documents/$ROOTFS_FILENAME" ;;
esac
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

# xcodebuild has no option for the test host's environment. It passes each of
# its own TEST_RUNNER_-prefixed variables to the test host with the prefix
# removed, so these are exported to xcodebuild rather than given as arguments.
set -- "TEST_RUNNER_KERNARIUM_ROOTFS=$LAUNCH_ROOTFS"
if [ "$AQUARIUM" -eq 1 ]; then
	# The automation runtime appends this payload to its boot and boots its
	# target; it replaces the payload an earlier run of this script left.
	PAYLOAD=kernarium-aquarium-payload.cpio
	rm -f -- "$DATA_CONTAINER/Documents/$PAYLOAD"
	python3 -B "$ROOT_DIR/tests/integration/gpu/run_chromium_webgl.py" \
		--kernarium-payload "$DATA_CONTAINER/Documents/$PAYLOAD" \
		--initramfs-tool "$INITRAMFS_TOOL"
	set -- "$@" \
		"TEST_RUNNER_KERNARIUM_BOOT_PAYLOAD=$PAYLOAD" \
		TEST_RUNNER_KERNARIUM_SYSTEMD_UNIT=uml-aquarium.target \
		TEST_RUNNER_KERNARIUM_EXECUTION_ENGINE=gadgets \
		TEST_RUNNER_KERNARIUM_VCPU_COUNT=4 \
		TEST_RUNNER_KERNARIUM_GUEST_MEMORY_MIB=4096 \
		TEST_RUNNER_KERNARIUM_OVERLAY_SIZE_GIB=8 \
		TEST_RUNNER_KERNARIUM_RETINA_MODE=0 \
		TEST_RUNNER_UML_GPU_FRAME_STATS=1
fi

# `open` cannot start an iOS application on macOS and `devicectl` does not
# enumerate the host Mac, so Xcode's local install service is the only
# command-line launcher for this mode and `xcodebuild test` is what drives it.
# The app hosts its own test bundle, so this both launches the application and
# checks the contracts that depend on the mode. The launched product stays
# optimized; testability is raised only so the hosted bundle can import it.
printf '%s\n' "launching Kernarium on this Mac in Designed for iPad mode"
exec env "$@" xcodebuild \
	-project "$ROOT_DIR/Kernarium/Kernarium.xcodeproj" \
	-scheme Kernarium \
	-configuration Release \
	-destination "$DESTINATION" \
	-derivedDataPath "$DERIVED_DATA" \
	-jobs "$JOBS" \
	-only-testing:KernariumTests \
	ENABLE_TESTABILITY=YES \
	test
