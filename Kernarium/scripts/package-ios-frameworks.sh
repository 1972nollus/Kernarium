#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Combine the verified Simulator and device framework slices used by the
# Kernarium Xcode project. All output is generated below .build by default.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUTPUT=${KERNARIUM_XCFRAMEWORK_OUTPUT:-"$ROOT_DIR/.build/kernarium-xcframeworks"}

fail()
{
	printf '%s\n' "Kernarium/scripts/package-ios-frameworks.sh: $*" >&2
	exit 1
}

if [ "$#" -ne 0 ]; then
	fail "this script takes no arguments"
fi

find_tool()
{
	variable=$1
	shift
	for candidate in "$@"; do
		tool_path=$(command -v "$candidate" 2>/dev/null || true)
		if [ -n "$tool_path" ]; then
			eval "$variable=\$tool_path"
			return
		fi
	done
	fail "required packaging tool is unavailable: $*"
}

find_tool XCODEBUILD xcodebuild
find_tool CMAKE cmake

RUNTIME_4K="KernariumRuntime_6_18_4K"
RUNTIME_16K="KernariumRuntime_6_18_16K"

# The Android runtime carries an independently accepted ACK artifact. Rootfs
# release updates do not rename this framework and kernel-only updates do not
# invalidate an existing guest bundle.
RUNTIME_ANDROID_16K="KernariumRuntimeAndroid_6_18_16K"

SIMULATOR_RUNTIME_4K="$ROOT_DIR/.build/kernarium-runtime-ios-simulator-4k/Kernarium/Runtime/$RUNTIME_4K.framework"
SIMULATOR_RUNTIME_16K="$ROOT_DIR/.build/kernarium-runtime-ios-simulator-16k/Kernarium/Runtime/$RUNTIME_16K.framework"
DEVICE_RUNTIME_4K="$ROOT_DIR/.build/kernarium-runtime-ios-device-4k/Kernarium/Runtime/$RUNTIME_4K.framework"
DEVICE_RUNTIME_16K="$ROOT_DIR/.build/kernarium-runtime-ios-device-16k/Kernarium/Runtime/$RUNTIME_16K.framework"
SIMULATOR_RUNTIME_ANDROID="$ROOT_DIR/.build/kernarium-runtime-ios-simulator-android-16k/Kernarium/Runtime/$RUNTIME_ANDROID_16K.framework"
DEVICE_RUNTIME_ANDROID="$ROOT_DIR/.build/kernarium-runtime-ios-device-android-16k/Kernarium/Runtime/$RUNTIME_ANDROID_16K.framework"
SIMULATOR_GPU="$ROOT_DIR/.build/dependencies/apple-gpu/ios-simulator-arm64/Frameworks"
DEVICE_GPU="$ROOT_DIR/.build/dependencies/apple-gpu/ios-device-arm64/Frameworks"

# Packaging is an independent trust boundary: revalidate the complete source,
# contract, provenance, binary-platform, and provider-negative policy before
# consuming either platform prefix.
"$ROOT_DIR/dependencies/build-apple-gpu.sh" \
	--platform ios-simulator --verify-only
"$ROOT_DIR/dependencies/build-apple-gpu.sh" \
	--platform ios --verify-only

for framework in \
	"$SIMULATOR_RUNTIME_4K" \
	"$SIMULATOR_RUNTIME_16K" \
	"$SIMULATOR_RUNTIME_ANDROID" \
	"$DEVICE_RUNTIME_4K" \
	"$DEVICE_RUNTIME_16K" \
	"$DEVICE_RUNTIME_ANDROID" \
	"$SIMULATOR_GPU/EGL.framework" \
	"$DEVICE_GPU/EGL.framework" \
	"$SIMULATOR_GPU/GLESv2.framework" \
	"$DEVICE_GPU/GLESv2.framework"; do
	[ -d "$framework" ] || fail "missing framework slice: $framework"
done

OUTPUT_PARENT=$(dirname -- "$OUTPUT")
mkdir -p -- "$OUTPUT_PARENT"
OUTPUT_PARENT=$(CDPATH= cd -- "$OUTPUT_PARENT" && pwd -P)
OUTPUT="$OUTPUT_PARENT/$(basename -- "$OUTPUT")"
STAGE=$(mktemp -d "$OUTPUT_PARENT/.kernarium-xcframeworks.XXXXXX")
cleanup()
{
	if [ -n "${STAGE:-}" ] && [ -d "$STAGE" ]; then
		"$CMAKE" -E remove_directory "$STAGE"
	fi
}
trap cleanup EXIT HUP INT TERM

create_xcframework()
{
	name=$1
	simulator_framework=$2
	device_framework=$3
	"$XCODEBUILD" -create-xcframework \
		-framework "$simulator_framework" \
		-framework "$device_framework" \
		-output "$STAGE/$name.xcframework"
	[ -f "$STAGE/$name.xcframework/Info.plist" ] ||
		fail "$name XCFramework was not created"
}

create_xcframework "$RUNTIME_4K" \
	"$SIMULATOR_RUNTIME_4K" "$DEVICE_RUNTIME_4K"
create_xcframework "$RUNTIME_16K" \
	"$SIMULATOR_RUNTIME_16K" "$DEVICE_RUNTIME_16K"
create_xcframework "$RUNTIME_ANDROID_16K" \
	"$SIMULATOR_RUNTIME_ANDROID" "$DEVICE_RUNTIME_ANDROID"
create_xcframework EGL \
	"$SIMULATOR_GPU/EGL.framework" "$DEVICE_GPU/EGL.framework"
create_xcframework GLESv2 \
	"$SIMULATOR_GPU/GLESv2.framework" "$DEVICE_GPU/GLESv2.framework"

for platform in ios-arm64 ios-arm64-simulator; do
	for framework in "$RUNTIME_4K" "$RUNTIME_16K" \
		"$RUNTIME_ANDROID_16K" EGL GLESv2; do
		binary="$STAGE/$framework.xcframework/$platform/$framework.framework/$framework"
		[ -f "$binary" ] || fail "missing packaged framework binary: $binary"
	done
done

if [ -e "$OUTPUT" ] || [ -L "$OUTPUT" ]; then
	"$CMAKE" -E remove_directory "$OUTPUT"
fi
mv "$STAGE" "$OUTPUT"
STAGE=
printf '%s\n' "published Kernarium XCFrameworks: $OUTPUT"
