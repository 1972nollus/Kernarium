#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Package the verified iOS device framework slices used by the Kernarium
# Xcode project. All output is generated below .build by default.
#
# The packaged stack is device-only. Mesa Zink renders on KosmicKrisp, which
# requires Metal 4, and the iOS Simulator exposes no Metal 4 device, so there
# is no Simulator slice to combine. An iOS device build runs on an Apple
# silicon Mac in Designed for iPad mode when no device is attached.
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
find_tool OTOOL otool

RUNTIME_4K="KRuntime_6_18_4K"
RUNTIME_16K="KRuntime_6_18_16K"

# The Android runtime carries an independently accepted ACK artifact. Rootfs
# release updates do not rename this framework and kernel-only updates do not
# invalidate an existing guest bundle.
RUNTIME_ANDROID_16K="KRuntime_ACK_6_18_16K"

DEVICE_RUNTIME_4K="$ROOT_DIR/.build/kernarium-runtime-ios-device-4k/Kernarium/Runtime/$RUNTIME_4K.framework"
DEVICE_RUNTIME_16K="$ROOT_DIR/.build/kernarium-runtime-ios-device-16k/Kernarium/Runtime/$RUNTIME_16K.framework"
DEVICE_RUNTIME_ANDROID="$ROOT_DIR/.build/kernarium-runtime-ios-device-android-16k/Kernarium/Runtime/$RUNTIME_ANDROID_16K.framework"
DEVICE_GPU="$ROOT_DIR/.build/dependencies/apple-gpu-vulkan-experiment/ios-device-arm64/Frameworks"
STORAGE="$ROOT_DIR/.build/dependencies/ios-storage/GPTFDisk.xcframework"
ZSTD="$ROOT_DIR/.build/dependencies/ios-zstd/LibZstd.xcframework"

# Mesa supplies the host GL provider as MesaEGL and MesaGallium, and the
# Vulkan driver they render on as KosmicKrisp.
GPU_FRAMEWORKS="MesaEGL MesaGallium KosmicKrisp"

# Packaging is an independent trust boundary: revalidate the complete source,
# contract, provenance, binary-platform, and provider-negative policy before
# consuming the device prefix.
"$ROOT_DIR/dependencies/build-apple-gpu.sh" --platform ios --verify-only
"$ROOT_DIR/dependencies/build-ios-storage.sh"
"$ROOT_DIR/dependencies/build-ios-zstd.sh"

for framework in \
	"$DEVICE_RUNTIME_4K" \
	"$DEVICE_RUNTIME_16K" \
	"$DEVICE_RUNTIME_ANDROID" \
	"$STORAGE" \
	"$ZSTD"; do
	[ -d "$framework" ] || fail "missing framework slice: $framework"
done
for framework in $GPU_FRAMEWORKS; do
	[ -d "$DEVICE_GPU/$framework.framework" ] ||
		fail "missing framework slice: $DEVICE_GPU/$framework.framework"
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
	device_framework=$2
	"$XCODEBUILD" -create-xcframework \
		-framework "$device_framework" \
		-output "$STAGE/$name.xcframework"
	[ -f "$STAGE/$name.xcframework/Info.plist" ] ||
		fail "$name XCFramework was not created"
}

create_xcframework "$RUNTIME_4K" "$DEVICE_RUNTIME_4K"
create_xcframework "$RUNTIME_16K" "$DEVICE_RUNTIME_16K"
create_xcframework "$RUNTIME_ANDROID_16K" "$DEVICE_RUNTIME_ANDROID"
for framework in $GPU_FRAMEWORKS; do
	create_xcframework "$framework" "$DEVICE_GPU/$framework.framework"
done
"$CMAKE" -E copy_directory "$STORAGE" \
	"$STAGE/GPTFDisk.xcframework"
"$CMAKE" -E copy_directory "$ZSTD" \
	"$STAGE/LibZstd.xcframework"

RUNTIME_FRAMEWORKS="$RUNTIME_4K $RUNTIME_16K $RUNTIME_ANDROID_16K"
for framework in $RUNTIME_FRAMEWORKS $GPU_FRAMEWORKS; do
	binary="$STAGE/$framework.xcframework/ios-arm64/$framework.framework/$framework"
	[ -f "$binary" ] || fail "missing packaged framework binary: $binary"
done
for framework in $RUNTIME_FRAMEWORKS; do
	binary="$STAGE/$framework.xcframework/ios-arm64/$framework.framework/$framework"
	rpaths=$("$OTOOL" -l "$binary" | awk '
		$1 == "cmd" { command = $2; next }
		command == "LC_RPATH" && $1 == "path" {
			print $2
			command = ""
		}
	')
	[ "$rpaths" = '@loader_path/..' ] ||
		fail "$framework has an invalid runtime search path: $rpaths"
done

# The storage and compression cores carry no Metal dependency, so their
# XCFrameworks keep both published slices.
for platform in ios-arm64 ios-arm64-simulator; do
	binary="$STAGE/GPTFDisk.xcframework/$platform/GPTFDisk.framework/GPTFDisk"
	[ -f "$binary" ] || fail "missing packaged GPT fdisk core: $binary"
	binary="$STAGE/LibZstd.xcframework/$platform/LibZstd.framework/LibZstd"
	[ -f "$binary" ] || fail "missing packaged zstd decompressor: $binary"
done

if [ -e "$OUTPUT" ] || [ -L "$OUTPUT" ]; then
	"$CMAKE" -E remove_directory "$OUTPUT"
fi
mv "$STAGE" "$OUTPUT"
STAGE=
printf '%s\n' "published Kernarium XCFrameworks: $OUTPUT"
