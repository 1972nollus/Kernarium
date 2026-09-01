#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the pinned zstd decompressor as one static XCFramework containing
# arm64 Simulator and device slices. The script is offline and consumes only
# sources authenticated by dependencies/prepare.sh.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
OUTPUT=${UML_IOS_ZSTD_DEPENDENCY_PREFIX:-"$ROOT_DIR/.build/dependencies/ios-zstd"}
WORK_ROOT=${UML_IOS_ZSTD_DEPENDENCY_BUILD_ROOT:-"$ROOT_DIR/.build/dependency-build/ios-zstd"}
VERIFY_SCRIPT="$ROOT_DIR/dependencies/cmake/VerifyIosZstdXCFramework.cmake"
JOBS=${UML_IOS_ZSTD_DEPENDENCY_JOBS:-8}
DEPLOYMENT_TARGET=16.0
VERIFY_ONLY=0

fail()
{
	printf '%s\n' "dependencies/build-ios-zstd.sh: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: dependencies/build-ios-zstd.sh [options]" \
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
[ "$(uname -s)" = Darwin ] || fail "the iOS zstd build requires macOS"
case "$(uname -m)" in
arm64|aarch64) ;;
*) fail "the iOS zstd build requires an arm64 host" ;;
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
	printf '%s\n' "verified iOS zstd XCFramework: $OUTPUT"
	exit 0
fi

if [ -e "$OUTPUT" ] || [ -L "$OUTPUT" ]; then
	verify_output "$OUTPUT" ||
		fail "existing output is invalid and was left untouched: $OUTPUT"
	printf '%s\n' "reusing verified iOS zstd XCFramework: $OUTPUT"
	exit 0
fi

"$ROOT_DIR/dependencies/prepare.sh" --product ios-compression \
	--root "$SOURCE_ROOT" --verify
SOURCE="$SOURCE_ROOT/zstd"
[ -f "$SOURCE/lib/Makefile" ] || fail "prepared zstd source is missing"

mkdir -p -- "$WORK_ROOT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
TEMP_ROOT=$(mktemp -d "$WORK_ROOT/source-build.XXXXXX")
STAGE=$(mktemp -d "$OUTPUT_PARENT/.ios-zstd-prefix.XXXXXX")
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
	library="$build_dir/static/libzstd.a"
	framework="$TEMP_ROOT/framework-$platform/LibZstd.framework"
	sdk_root=$($XCRUN --sdk "$sdk_name" --show-sdk-path)
	cc=$($XCRUN --sdk "$sdk_name" --find clang)
	ar=$($XCRUN --sdk "$sdk_name" --find ar)
	ranlib=$($XCRUN --sdk "$sdk_name" --find ranlib)

	"$MAKE" -C "$SOURCE/lib" \
		BUILD_DIR="$build_dir" \
		CC="$cc" AR="$ar" RANLIB="$ranlib" \
		CPPFLAGS="-DZSTD_LEGACY_SUPPORT=0 -DZSTD_DISABLE_ASM=1" \
		CFLAGS="-target $target -isysroot $sdk_root -O3 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -Wall -Wextra -Werror" \
		ZSTD_LEGACY_SUPPORT=0 \
		ZSTD_LIB_COMPRESSION=0 \
		ZSTD_LIB_DECOMPRESSION=1 \
		ZSTD_LIB_DICTBUILDER=0 \
		ZSTD_LIB_DEPRECATED=0 \
		-j "$JOBS" "$library"
	[ -f "$library" ] || fail "zstd archive was not created for $platform"
	mkdir -p -- "$framework/Headers" "$framework/Modules"
	cp "$library" "$framework/LibZstd"
	cp "$SOURCE/lib/zstd.h" "$framework/Headers/zstd.h"
	cp "$SOURCE/lib/zstd_errors.h" "$framework/Headers/zstd_errors.h"
	cp "$ROOT_DIR/dependencies/zstd/module.modulemap" \
		"$framework/Modules/module.modulemap"
	"$CMAKE" -DFRAMEWORK_NAME=LibZstd \
		-DOUTPUT="$framework/Info.plist" \
		-P "$ROOT_DIR/dependencies/cmake/ConfigureStaticFrameworkPlist.cmake"
}

build_slice ios iphoneos "arm64-apple-ios$DEPLOYMENT_TARGET"
build_slice ios-simulator iphonesimulator \
	"arm64-apple-ios$DEPLOYMENT_TARGET-simulator"

"$XCODEBUILD" -create-xcframework \
	-framework "$TEMP_ROOT/framework-ios/LibZstd.framework" \
	-framework "$TEMP_ROOT/framework-ios-simulator/LibZstd.framework" \
	-output "$STAGE/LibZstd.xcframework"

PROVENANCE="$STAGE/share/uml-dependencies"
mkdir -p -- "$PROVENANCE"
cp "$ROOT_DIR/dependencies/manifest.tsv" "$PROVENANCE/manifest.tsv"
cp "$ROOT_DIR/dependencies/platforms/ios-device-compression-arm64.tsv" \
	"$PROVENANCE/ios-device-compression-arm64.tsv"
cp "$ROOT_DIR/dependencies/platforms/ios-simulator-compression-arm64.tsv" \
	"$PROVENANCE/ios-simulator-compression-arm64.tsv"

verify_output "$STAGE"
mv "$STAGE" "$OUTPUT"
STAGE=
printf '%s\n' "published iOS zstd XCFramework: $OUTPUT"
