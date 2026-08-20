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

. "$ROOT_DIR/kernel/mainline-6.18/patches/metadata.env"
case "$UML_KERNEL_RELEASE_VERSION" in
''|*[!0-9.]*|.*|*..*|*.) fail "invalid kernel release identity" ;;
esac
RUNTIME_RELEASE=$(printf '%s\n' "$UML_KERNEL_RELEASE_VERSION" | tr . _)
RUNTIME_4K="KernariumRuntime_${RUNTIME_RELEASE}_4K"
RUNTIME_16K="KernariumRuntime_${RUNTIME_RELEASE}_16K"

SIMULATOR_RUNTIME_4K="$ROOT_DIR/.build/kernarium-runtime-ios-simulator-4k/Kernarium/Runtime/$RUNTIME_4K.framework"
SIMULATOR_RUNTIME_16K="$ROOT_DIR/.build/kernarium-runtime-ios-simulator-16k/Kernarium/Runtime/$RUNTIME_16K.framework"
DEVICE_RUNTIME_4K="$ROOT_DIR/.build/kernarium-runtime-ios-device-4k/Kernarium/Runtime/$RUNTIME_4K.framework"
DEVICE_RUNTIME_16K="$ROOT_DIR/.build/kernarium-runtime-ios-device-16k/Kernarium/Runtime/$RUNTIME_16K.framework"
SIMULATOR_GPU="$ROOT_DIR/.build/dependencies/ios-simulator-arm64/gpu/Frameworks"
DEVICE_GPU="$ROOT_DIR/.build/dependencies/ios-device-arm64/gpu/Frameworks"

for framework in \
	"$SIMULATOR_RUNTIME_4K" \
	"$SIMULATOR_RUNTIME_16K" \
	"$DEVICE_RUNTIME_4K" \
	"$DEVICE_RUNTIME_16K" \
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
create_xcframework EGL \
	"$SIMULATOR_GPU/EGL.framework" "$DEVICE_GPU/EGL.framework"
create_xcframework GLESv2 \
	"$SIMULATOR_GPU/GLESv2.framework" "$DEVICE_GPU/GLESv2.framework"

for platform in ios-arm64 ios-arm64-simulator; do
	for framework in "$RUNTIME_4K" "$RUNTIME_16K" EGL GLESv2; do
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
