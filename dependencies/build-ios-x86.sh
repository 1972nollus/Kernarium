#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the authenticated x86 semantic support archives for arm64 iOS.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
PREFIX=${UML_IOS_X86_DEPENDENCY_PREFIX:-}
WORK_ROOT=${UML_IOS_X86_DEPENDENCY_BUILD_ROOT:-"$ROOT_DIR/.build/dependency-build/ios-x86"}
VERIFY_SCRIPT="$ROOT_DIR/dependencies/cmake/VerifyIosX86Prefix.cmake"
JOBS=${UML_IOS_X86_DEPENDENCY_JOBS:-8}
DEPLOYMENT_TARGET=16.0
PLATFORM=ios-simulator
VERIFY_ONLY=0

fail()
{
	printf '%s\n' "dependencies/build-ios-x86.sh: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: dependencies/build-ios-x86.sh [options]" \
		"" \
		"Options:" \
		"  --platform NAME     ios-simulator (default) or ios" \
		"  --source-root PATH  Verified dependency source trees" \
		"  --prefix PATH       Output prefix" \
		"  --work-root PATH    Temporary build parent" \
		"  --jobs NUMBER       Parallel build jobs (default: 8)" \
		"  --verify-only       Verify an existing prefix" \
		"  -h, --help          Show this help"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--platform)
		[ "$#" -ge 2 ] || fail "--platform requires ios or ios-simulator"
		PLATFORM=$2
		shift 2
		;;
	--source-root)
		[ "$#" -ge 2 ] || fail "--source-root requires a path"
		SOURCE_ROOT=$2
		shift 2
		;;
	--prefix)
		[ "$#" -ge 2 ] || fail "--prefix requires a path"
		PREFIX=$2
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
[ "$(uname -s)" = Darwin ] || fail "the iOS x86 support build requires macOS"
case "$(uname -m)" in
arm64|aarch64) ;;
*) fail "the iOS x86 support build requires an arm64 host" ;;
esac
case "$PLATFORM" in
ios-simulator)
	SDK_NAME=iphonesimulator
	RECEIPT_NAME=ios-simulator-x86-arm64.tsv
	DEFAULT_PREFIX="$ROOT_DIR/.build/dependencies/ios-simulator-arm64/x86"
	;;
ios)
	SDK_NAME=iphoneos
	RECEIPT_NAME=ios-device-x86-arm64.tsv
	DEFAULT_PREFIX="$ROOT_DIR/.build/dependencies/ios-device-arm64/x86"
	;;
*) fail "platform must be ios or ios-simulator" ;;
esac
RECEIPT="$ROOT_DIR/dependencies/platforms/$RECEIPT_NAME"
[ -r "$RECEIPT" ] || fail "platform receipt is unavailable: $RECEIPT"
if [ -z "$PREFIX" ]; then
	PREFIX=$DEFAULT_PREFIX
fi
PREFIX_PARENT=$(dirname -- "$PREFIX")
mkdir -p -- "$PREFIX_PARENT"
PREFIX_PARENT=$(CDPATH= cd -- "$PREFIX_PARENT" && pwd -P)
PREFIX="$PREFIX_PARENT/$(basename -- "$PREFIX")"

verify_prefix()
{
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" \
		-DPLATFORM="$PLATFORM" -P "$VERIFY_SCRIPT"
}

if [ "$VERIFY_ONLY" -eq 1 ]; then
	verify_prefix "$PREFIX"
	printf '%s\n' "verified iOS x86 support prefix: $PREFIX"
	exit 0
fi
if [ -e "$PREFIX" ] || [ -L "$PREFIX" ]; then
	verify_prefix "$PREFIX" ||
		fail "existing prefix is invalid and was left untouched: $PREFIX"
	printf '%s\n' "reusing verified iOS x86 support prefix: $PREFIX"
	exit 0
fi

"$ROOT_DIR/dependencies/prepare.sh" --product ios-x86 \
	--root "$SOURCE_ROOT" --verify
command -v cmake >/dev/null 2>&1 || fail "cmake is required"
command -v ninja >/dev/null 2>&1 || fail "ninja is required"
command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
xcrun --sdk "$SDK_NAME" --show-sdk-path >/dev/null

mkdir -p -- "$WORK_ROOT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
BUILD_DIR=$(mktemp -d "$WORK_ROOT/source-build.XXXXXX")
STAGE=$(mktemp -d "$PREFIX_PARENT/.ios-x86-prefix.XXXXXX")
cleanup()
{
	if [ -n "${BUILD_DIR:-}" ] && [ -d "$BUILD_DIR" ]; then
		rm -rf -- "$BUILD_DIR"
	fi
	if [ -n "${STAGE:-}" ] && [ -d "$STAGE" ]; then
		rm -rf -- "$STAGE"
	fi
}
trap cleanup EXIT HUP INT TERM

printf '%s\n' "building Berkeley SoftFloat 3e and 128-bit Cephes for $PLATFORM"
cmake -S "$ROOT_DIR/dependencies/softfloat" -B "$BUILD_DIR" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$STAGE" \
	-DCMAKE_SYSTEM_NAME=iOS \
	-DCMAKE_OSX_SYSROOT="$SDK_NAME" \
	-DCMAKE_OSX_ARCHITECTURES=arm64 \
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
	-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
	-DUML_FEX_SOURCE_DIR="$SOURCE_ROOT/fex"
cmake --build "$BUILD_DIR" --parallel "$JOBS"
cmake --install "$BUILD_DIR"
mkdir -p -- "$STAGE/share/uml-dependencies"
cp "$ROOT_DIR/dependencies/manifest.tsv" \
	"$STAGE/share/uml-dependencies/manifest.tsv"
cp "$RECEIPT" "$STAGE/share/uml-dependencies/$RECEIPT_NAME"
verify_prefix "$STAGE"
mv "$STAGE" "$PREFIX"
STAGE=
printf '%s\n' "published iOS x86 support prefix: $PREFIX"
