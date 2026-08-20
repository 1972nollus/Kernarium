#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the one ANGLE/Metal, libepoxy, and virglrenderer product used by every
# Apple platform from sources authenticated by dependencies/prepare.sh.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MANIFEST="$ROOT_DIR/dependencies/manifest.tsv"
SOURCE_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
VERIFY_SCRIPT="$ROOT_DIR/dependencies/cmake/VerifyAppleGpuPrefix.cmake"
PREFIX=${UML_APPLE_GPU_DEPENDENCY_PREFIX:-}
WORK_ROOT=${UML_APPLE_GPU_DEPENDENCY_BUILD_ROOT:-}
JOBS=${UML_APPLE_GPU_DEPENDENCY_JOBS:-8}
PLATFORM=macos
VERIFY_ONLY=0

fail()
{
	printf '%s\n' "dependencies/build-apple-gpu.sh: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: dependencies/build-apple-gpu.sh [options]" \
		"" \
		"Options:" \
		"  --platform NAME      macos (default), ios-simulator, or ios" \
		"  --source-root PATH   Verified dependency source trees" \
		"  --prefix PATH        Output prefix" \
		"  --work-root PATH     Temporary build parent" \
		"  --jobs NUMBER        Parallel build jobs (default: 8)" \
		"  --verify-only        Verify an existing prefix" \
		"  -h, --help           Show this help" \
		"" \
		"The build performs no network access. Run dependencies/prepare.sh" \
		"--product apple-gpu first to materialize the pinned source matrix."
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--platform)
		[ "$#" -ge 2 ] || fail "--platform requires a value"
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
[ "$(uname -s)" = Darwin ] || fail "the Apple GPU build requires macOS"
case "$(uname -m)" in
arm64|aarch64) ;;
*) fail "the Apple GPU build requires an arm64 host" ;;
esac

case "$PLATFORM" in
macos)
	SDK_NAME=macosx
	TARGET=arm64-apple-macos14.0
	EPOXY_SYSTEM=darwin
	SUBSYSTEM=macos
	DEPLOYMENT_TARGET=14.0
	DEPLOYMENT_SETTING=MACOSX_DEPLOYMENT_TARGET
	CONTRACT_NAME=macos-gpu-arm64.tsv
	MINIMUM_OS_KEY=LSMinimumSystemVersion
	DEFAULT_PREFIX="$ROOT_DIR/.build/dependencies/apple-gpu/macos-arm64"
	;;
ios-simulator)
	SDK_NAME=iphonesimulator
	TARGET=arm64-apple-ios16.0-simulator
	EPOXY_SYSTEM=ios
	SUBSYSTEM=ios-simulator
	DEPLOYMENT_TARGET=16.0
	DEPLOYMENT_SETTING=IPHONEOS_DEPLOYMENT_TARGET
	CONTRACT_NAME=ios-simulator-gpu-arm64.tsv
	MINIMUM_OS_KEY=MinimumOSVersion
	DEFAULT_PREFIX="$ROOT_DIR/.build/dependencies/apple-gpu/ios-simulator-arm64"
	;;
ios)
	SDK_NAME=iphoneos
	TARGET=arm64-apple-ios16.0
	EPOXY_SYSTEM=ios
	SUBSYSTEM=ios
	DEPLOYMENT_TARGET=16.0
	DEPLOYMENT_SETTING=IPHONEOS_DEPLOYMENT_TARGET
	CONTRACT_NAME=ios-device-gpu-arm64.tsv
	MINIMUM_OS_KEY=MinimumOSVersion
	DEFAULT_PREFIX="$ROOT_DIR/.build/dependencies/apple-gpu/ios-device-arm64"
	;;
*) fail "platform must be macos, ios-simulator, or ios" ;;
esac

PLATFORM_CONTRACT="$ROOT_DIR/dependencies/platforms/$CONTRACT_NAME"
[ -r "$MANIFEST" ] || fail "source manifest is unavailable: $MANIFEST"
[ -r "$PLATFORM_CONTRACT" ] ||
	fail "platform contract is unavailable: $PLATFORM_CONTRACT"
[ -n "$PREFIX" ] || PREFIX=$DEFAULT_PREFIX
[ -n "$WORK_ROOT" ] ||
	WORK_ROOT="$ROOT_DIR/.build/dependency-build/apple-gpu/$PLATFORM"

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
	printf '%s\n' "verified $PLATFORM Apple GPU dependency prefix: $PREFIX"
	exit 0
fi

if [ -e "$PREFIX" ] || [ -L "$PREFIX" ]; then
	verify_prefix "$PREFIX" ||
		fail "existing prefix is invalid and was left untouched: $PREFIX"
	printf '%s\n' "reusing verified $PLATFORM Apple GPU prefix: $PREFIX"
	exit 0
fi

"$ROOT_DIR/dependencies/prepare.sh" --product apple-gpu \
	--root "$SOURCE_ROOT" --verify
WEBKIT_SOURCE="$SOURCE_ROOT/utm-webkit"
EPOXY_SOURCE="$SOURCE_ROOT/utm-libepoxy"
VIRGL_SOURCE="$SOURCE_ROOT/utm-virglrenderer"

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

if [ -x "$ROOT_DIR/.build/gpu-tools-venv/bin/meson" ]; then
	PATH="$ROOT_DIR/.build/gpu-tools-venv/bin:$PATH"
	export PATH
	MESON="$ROOT_DIR/.build/gpu-tools-venv/bin/meson"
else
	find_tool MESON meson
fi
find_tool RSYNC rsync
find_tool PKG_CONFIG pkg-config
find_tool XCODEBUILD xcodebuild
find_tool XCRUN xcrun
find_tool INSTALL_NAME_TOOL install_name_tool
find_tool PLUTIL plutil
find_tool PYTHON python3

SDK_ROOT=$($XCRUN --sdk "$SDK_NAME" --show-sdk-path)
[ -d "$SDK_ROOT" ] || fail "xcrun returned an invalid $PLATFORM SDK"
CC=$($XCRUN --sdk "$SDK_NAME" --find clang)
CXX=$($XCRUN --sdk "$SDK_NAME" --find clang++)
AR=$($XCRUN --sdk "$SDK_NAME" --find ar)
NM=$($XCRUN --sdk "$SDK_NAME" --find nm)
RANLIB=$($XCRUN --sdk "$SDK_NAME" --find ranlib)
STRIP=$($XCRUN --sdk "$SDK_NAME" --find strip)

mkdir -p -- "$WORK_ROOT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
TEMP_ROOT=$(mktemp -d "$WORK_ROOT/source-build.XXXXXX")
STAGE=$(mktemp -d "$PREFIX_PARENT/.apple-gpu-prefix.XXXXXX")
cleanup()
{
	if [ -n "${TEMP_ROOT:-}" ] && [ -d "$TEMP_ROOT" ]; then
		rm -rf -- "$TEMP_ROOT"
	fi
	if [ -n "${STAGE:-}" ] && [ -d "$STAGE" ]; then
		rm -rf -- "$STAGE"
	fi
}
trap cleanup EXIT HUP INT TERM

publish_exclusively()
{
	python3 - "$1" "$2" <<'PY'
import ctypes
import os
import sys


source = os.fsencode(sys.argv[1])
destination = os.fsencode(sys.argv[2])
libc = ctypes.CDLL(None, use_errno=True)
rename_exclusive = libc.renamex_np
rename_exclusive.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
rename_exclusive.restype = ctypes.c_int
raise SystemExit(rename_exclusive(source, destination, 0x00000004) != 0)
PY
}

SOURCES="$TEMP_ROOT/sources"
BUILDS="$TEMP_ROOT/builds"
ANGLE_ROOT="$SOURCES/WebKit/Source/ThirdParty/ANGLE"
ANGLE_ARCHIVE="$BUILDS/ANGLE"
EPOXY_CROSS="$BUILDS/meson-epoxy-$PLATFORM.cross"
VIRGL_CROSS="$BUILDS/meson-virgl-$PLATFORM.cross"
mkdir -p -- "$ANGLE_ROOT" "$SOURCES/WebKit/Configurations" \
	"$SOURCES/WebKit/Tools/ccache" "$SOURCES/libepoxy" \
	"$SOURCES/virglrenderer" "$BUILDS" "$STAGE/include" \
	"$STAGE/lib" "$STAGE/Frameworks"

$RSYNC -a --exclude .git --exclude ANGLE.xcarchive \
	"$WEBKIT_SOURCE/Source/ThirdParty/ANGLE/" "$ANGLE_ROOT/"
$RSYNC -a --exclude .git \
	"$WEBKIT_SOURCE/Configurations/" "$SOURCES/WebKit/Configurations/"
$RSYNC -a --exclude .git \
	"$WEBKIT_SOURCE/Tools/ccache/" "$SOURCES/WebKit/Tools/ccache/"
$RSYNC -a --exclude .git --exclude utm_build \
	"$EPOXY_SOURCE/" "$SOURCES/libepoxy/"
$RSYNC -a --exclude .git --exclude utm_build \
	"$VIRGL_SOURCE/" "$SOURCES/virglrenderer/"

APPLE_GPU_TEMP_DIR=${TMPDIR:-/tmp}
(
	cd "$ANGLE_ROOT"
	env -i PATH="$PATH" TMPDIR="$APPLE_GPU_TEMP_DIR" \
		"$XCODEBUILD" archive -quiet \
		-archivePath "$ANGLE_ARCHIVE" \
		-scheme ANGLE \
		-sdk "$SDK_NAME" \
		-arch arm64 \
		-configuration Release \
		WEBCORE_LIBRARY_DIR=/usr/local/lib \
		NORMAL_UMBRELLA_FRAMEWORKS_DIR= \
		CODE_SIGNING_ALLOWED=NO \
		"$DEPLOYMENT_SETTING=$DEPLOYMENT_TARGET" \
		WK_LIBCPP_ASSERTIONS_CFLAGS= \
		ANGLE_ALLOWABLE_CLIENTS= \
		GCC_TREAT_WARNINGS_AS_ERRORS=NO \
		OTHER_CPLUSPLUSFLAGS='$(inherited) -Wno-error=unnecessary-virtual-specifier'
)
[ -f "$ANGLE_ARCHIVE.xcarchive/Products/usr/local/lib/libEGL.dylib" ] ||
	fail "ANGLE archive did not produce libEGL.dylib"
[ -f "$ANGLE_ARCHIVE.xcarchive/Products/usr/local/lib/libGLESv2.dylib" ] ||
	fail "ANGLE archive did not produce libGLESv2.dylib"
for headers in EGL GLES GLES2 GLES3 KHR; do
	mkdir -p -- "$STAGE/include/$headers"
	$RSYNC -a "$ANGLE_ROOT/include/$headers/" "$STAGE/include/$headers/"
done

cat > "$EPOXY_CROSS" <<EOF
[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
cpp_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
objc_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
objcpp_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
c_link_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib']
cpp_link_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib']
objc_link_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib']
objcpp_link_args = ['-arch', 'arm64', '-target', '$TARGET', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib']

[binaries]
c = ['$CC']
cpp = ['$CXX']
objc = ['$CC']
objcpp = ['$CXX']
ar = ['$AR']
nm = ['$NM']
ranlib = ['$RANLIB']
strip = ['$STRIP', '-x']
pkg-config = ['$PKG_CONFIG']
python = ['$PYTHON']

[host_machine]
system = '$EPOXY_SYSTEM'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
EOF

awk -v source_system="$EPOXY_SYSTEM" -v subsystem="$SUBSYSTEM" '
	BEGIN {
		quote = sprintf("%c", 39)
		expected = "system = " quote source_system quote
	}
	$0 == expected {
		print "system = " quote "darwin" quote
		print "kernel = " quote "xnu" quote
		print "subsystem = " quote subsystem quote
		next
	}
	{ print }
' "$EPOXY_CROSS" > "$VIRGL_CROSS"

PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$STAGE/lib/pkgconfig" \
PKG_CONFIG_SYSROOT_DIR= \
	"$MESON" setup "$BUILDS/libepoxy" "$SOURCES/libepoxy" \
	--prefix "$STAGE" --buildtype release --cross-file "$EPOXY_CROSS" \
	--wrap-mode=nodownload \
	-Ddefault_library=static -Dtests=false -Dglx=no -Degl=yes -Dx11=false
"$MESON" compile -C "$BUILDS/libepoxy" -j "$JOBS"
"$MESON" install -C "$BUILDS/libepoxy"

PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$STAGE/lib/pkgconfig" \
PKG_CONFIG_SYSROOT_DIR= \
	"$MESON" setup "$BUILDS/virglrenderer" "$SOURCES/virglrenderer" \
	--prefix "$STAGE" --buildtype release --cross-file "$VIRGL_CROSS" \
	--wrap-mode=nodownload \
	-Ddefault_library=static -Dtests=false -Dvtest=false \
	-Dcheck-gl-errors=false -Dvenus=false -Dneptune=false \
	-Dplatforms=egl -Drender-server-mode=thread
"$MESON" compile -C "$BUILDS/virglrenderer" -j "$JOBS"
"$MESON" install -C "$BUILDS/virglrenderer"
install -m 0644 "$SOURCES/virglrenderer/src/virgl_hw.h" \
	"$STAGE/include/virgl/virgl_hw.h"

# CMake imports exact archives and headers. Build-only pkg-config metadata
# contains the exclusive staging path and must not enter the published prefix.
rm -rf -- "$STAGE/lib/pkgconfig"

install_framework()
{
	framework_name=$1
	source_binary=$2
	bundle_identifier=$3
	framework_dir="$STAGE/Frameworks/$framework_name.framework"
	mkdir -p -- "$framework_dir"
	install -m 0755 "$source_binary" "$framework_dir/$framework_name"
	"$INSTALL_NAME_TOOL" -id \
		"@rpath/$framework_name.framework/$framework_name" \
		"$framework_dir/$framework_name"
	cat > "$framework_dir/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>en</string>
	<key>CFBundleExecutable</key><string>$framework_name</string>
	<key>CFBundleIdentifier</key><string>$bundle_identifier</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>$framework_name</string>
	<key>CFBundlePackageType</key><string>FMWK</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>$MINIMUM_OS_KEY</key><string>$DEPLOYMENT_TARGET</string>
</dict>
</plist>
EOF
	"$PLUTIL" -lint "$framework_dir/Info.plist" >/dev/null
}

install_framework EGL \
	"$ANGLE_ARCHIVE.xcarchive/Products/usr/local/lib/libEGL.dylib" \
	org.linux-um-darwin.angle.EGL
install_framework GLESv2 \
	"$ANGLE_ARCHIVE.xcarchive/Products/usr/local/lib/libGLESv2.dylib" \
	org.linux-um-darwin.angle.GLESv2

mkdir -p "$STAGE/share/uml-dependencies/patches"
cp "$MANIFEST" "$STAGE/share/uml-dependencies/manifest.tsv"
cp -R "$ROOT_DIR/dependencies/patches/utm-webkit" \
	"$STAGE/share/uml-dependencies/patches/"
cp -R "$ROOT_DIR/dependencies/patches/utm-libepoxy" \
	"$STAGE/share/uml-dependencies/patches/"
cp -R "$ROOT_DIR/dependencies/patches/utm-virglrenderer" \
	"$STAGE/share/uml-dependencies/patches/"
cp "$PLATFORM_CONTRACT" \
	"$STAGE/share/uml-dependencies/$CONTRACT_NAME"
verify_prefix "$STAGE"
if ! publish_exclusively "$STAGE" "$PREFIX"; then
	if [ -e "$PREFIX" ] || [ -L "$PREFIX" ]; then
		verify_prefix "$PREFIX" ||
			fail "concurrent prefix winner is invalid: $PREFIX"
	else
		fail "Apple GPU prefix could not be published atomically: $PREFIX"
	fi
else
	STAGE=
fi
printf '%s\n' "published $PLATFORM Apple GPU dependency prefix: $PREFIX"
