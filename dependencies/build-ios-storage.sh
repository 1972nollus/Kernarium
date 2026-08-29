#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the patch-managed GPT fdisk core as one static XCFramework containing
# arm64 Simulator and device slices. The script is offline and consumes only
# sources authenticated by dependencies/prepare.sh.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
OUTPUT=${UML_IOS_STORAGE_DEPENDENCY_PREFIX:-"$ROOT_DIR/.build/dependencies/ios-storage"}
WORK_ROOT=${UML_IOS_STORAGE_DEPENDENCY_BUILD_ROOT:-"$ROOT_DIR/.build/dependency-build/ios-storage"}
VERIFY_SCRIPT="$ROOT_DIR/dependencies/cmake/VerifyIosStorageXCFramework.cmake"
JOBS=${UML_IOS_STORAGE_DEPENDENCY_JOBS:-8}
DEPLOYMENT_TARGET=16.0
VERIFY_ONLY=0

fail()
{
	printf '%s\n' "dependencies/build-ios-storage.sh: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: dependencies/build-ios-storage.sh [options]" \
		"" \
		"Options:" \
		"  --source-root PATH  Verified dependency source trees" \
		"  --output PATH       Output XCFramework prefix" \
		"  --work-root PATH    Temporary build parent" \
		"  --jobs NUMBER       Parallel build jobs (default: 8)" \
		"  --verify-only       Verify an existing XCFramework prefix" \
		"  -h, --help          Show this help"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--source-root)
		[ "$#" -ge 2 ] || fail "--source-root requires a path"
		SOURCE_ROOT=$2
		shift 2
		;;
	--output)
		[ "$#" -ge 2 ] || fail "--output requires a path"
		OUTPUT=$2
		shift 2
		;;
	--work-root)
		[ "$#" -ge 2 ] || fail "--work-root requires a path"
		WORK_ROOT=$2
		shift 2
		;;
	--jobs)
		[ "$#" -ge 2 ] || fail "--jobs requires a positive integer"
		JOBS=$2
		shift 2
		;;
	--verify-only)
		VERIFY_ONLY=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

case "$JOBS" in
''|*[!0-9]*) fail "jobs must be a positive integer" ;;
esac
[ "$JOBS" -gt 0 ] || fail "jobs must be a positive integer"
[ "$(uname -s)" = Darwin ] || fail "the iOS storage build requires macOS"
case "$(uname -m)" in
arm64|aarch64) ;;
*) fail "the iOS storage build requires an arm64 host" ;;
esac

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
	fail "required build tool is unavailable: $*"
}

find_tool CMAKE cmake
find_tool MAKE gmake make
find_tool XCODEBUILD xcodebuild
find_tool XCRUN xcrun

OUTPUT_PARENT=$(dirname -- "$OUTPUT")
mkdir -p -- "$OUTPUT_PARENT"
OUTPUT_PARENT=$(CDPATH= cd -- "$OUTPUT_PARENT" && pwd -P)
OUTPUT="$OUTPUT_PARENT/$(basename -- "$OUTPUT")"

verify_output()
{
	"$CMAKE" -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" -P "$VERIFY_SCRIPT"
}

if [ "$VERIFY_ONLY" -eq 1 ]; then
	verify_output "$OUTPUT"
	printf '%s\n' "verified iOS storage XCFramework: $OUTPUT"
	exit 0
fi

if [ -e "$OUTPUT" ] || [ -L "$OUTPUT" ]; then
	verify_output "$OUTPUT" ||
		fail "existing output is invalid and was left untouched: $OUTPUT"
	printf '%s\n' "reusing verified iOS storage XCFramework: $OUTPUT"
	exit 0
fi

"$ROOT_DIR/dependencies/prepare.sh" --product ios-storage \
	--root "$SOURCE_ROOT" --verify
SOURCE="$SOURCE_ROOT/gptfdisk"
[ -f "$SOURCE/Makefile.kernarium" ] || fail "prepared GPT fdisk core source is missing"

mkdir -p -- "$WORK_ROOT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
TEMP_ROOT=$(mktemp -d "$WORK_ROOT/source-build.XXXXXX")
STAGE=$(mktemp -d "$OUTPUT_PARENT/.ios-storage-prefix.XXXXXX")
cleanup()
{
	if [ -n "${TEMP_ROOT:-}" ] && [ -d "$TEMP_ROOT" ]; then
		"$CMAKE" -E remove_directory "$TEMP_ROOT"
	fi
	if [ -n "${STAGE:-}" ] && [ -d "$STAGE" ]; then
		"$CMAKE" -E remove_directory "$STAGE"
	fi
}
trap cleanup EXIT HUP INT TERM

build_slice()
{
	platform=$1
	sdk_name=$2
	target=$3
	build_dir="$TEMP_ROOT/build-$platform"
	library="$build_dir/libGPTFDisk.a"
	framework="$TEMP_ROOT/framework-$platform/GPTFDisk.framework"
	sdk_root=$($XCRUN --sdk "$sdk_name" --show-sdk-path)
	cxx=$($XCRUN --sdk "$sdk_name" --find clang++)
	ar=$($XCRUN --sdk "$sdk_name" --find ar)
	ranlib=$($XCRUN --sdk "$sdk_name" --find ranlib)

	"$MAKE" -C "$SOURCE" -f Makefile.kernarium \
		BUILD_DIR="$build_dir" LIBRARY="$library" \
		CXX="$cxx" AR="$ar" RANLIB="$ranlib" \
		CPPFLAGS=-DKERNARIUM_GPTFDISK_FILE_ONLY=1 \
		CXXFLAGS="-target $target -isysroot $sdk_root -O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -std=c++17 -Wall -Wextra -Werror" \
		-j "$JOBS"
	mkdir -p -- "$framework/Headers" "$framework/Modules"
	cp "$library" "$framework/GPTFDisk"
	cp "$SOURCE/kernarium_gptfdisk.h" "$framework/Headers/GPTFDisk.h"
	cp "$ROOT_DIR/dependencies/gptfdisk/module.modulemap" \
		"$framework/Modules/module.modulemap"
	"$CMAKE" -DFRAMEWORK_NAME=GPTFDisk \
		-DOUTPUT="$framework/Info.plist" \
		-P "$ROOT_DIR/dependencies/cmake/ConfigureStaticFrameworkPlist.cmake"
}

build_slice ios iphoneos "arm64-apple-ios$DEPLOYMENT_TARGET"
build_slice ios-simulator iphonesimulator \
	"arm64-apple-ios$DEPLOYMENT_TARGET-simulator"

"$XCODEBUILD" -create-xcframework \
	-framework "$TEMP_ROOT/framework-ios/GPTFDisk.framework" \
	-framework "$TEMP_ROOT/framework-ios-simulator/GPTFDisk.framework" \
	-output "$STAGE/GPTFDisk.xcframework"

PROVENANCE="$STAGE/share/uml-dependencies"
mkdir -p -- "$PROVENANCE/patches/gptfdisk"
cp "$ROOT_DIR/dependencies/manifest.tsv" "$PROVENANCE/manifest.tsv"
cp "$ROOT_DIR/dependencies/platforms/ios-device-storage-arm64.tsv" \
	"$PROVENANCE/ios-device-storage-arm64.tsv"
cp "$ROOT_DIR/dependencies/platforms/ios-simulator-storage-arm64.tsv" \
	"$PROVENANCE/ios-simulator-storage-arm64.tsv"
cp "$ROOT_DIR/dependencies/patches/gptfdisk/series.tsv" \
	"$PROVENANCE/patches/gptfdisk/series.tsv"
cp "$ROOT_DIR"/dependencies/patches/gptfdisk/*.patch \
	"$PROVENANCE/patches/gptfdisk/"

verify_output "$STAGE"
mv "$STAGE" "$OUTPUT"
STAGE=
printf '%s\n' "published iOS storage XCFramework: $OUTPUT"
