#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the exact UTM ANGLE/libepoxy/virglrenderer graphics stack used by
# Kernarium on iOS from sources authenticated by dependencies/prepare.sh.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MANIFEST="$ROOT_DIR/dependencies/manifest.tsv"
SOURCE_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
VERIFY_SCRIPT="$ROOT_DIR/dependencies/cmake/VerifyIosGpuPrefix.cmake"
PREFIX=${UML_IOS_GPU_DEPENDENCY_PREFIX:-}
WORK_ROOT=${UML_IOS_GPU_DEPENDENCY_BUILD_ROOT:-"$ROOT_DIR/.build/dependency-build/ios-gpu"}
JOBS=${UML_IOS_GPU_DEPENDENCY_JOBS:-8}
DEPLOYMENT_TARGET=16.0
PLATFORM=ios-simulator
VERIFY_ONLY=0

fail()
{
	printf '%s\n' "dependencies/build-ios-gpu.sh: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: dependencies/build-ios-gpu.sh [options]" \
		"" \
		"Options:" \
		"  --platform NAME      ios-simulator (default) or ios" \
		"  --source-root PATH    Verified dependency source trees" \
		"  --prefix PATH         Output prefix" \
		"  --work-root PATH      Temporary build parent" \
		"  --jobs NUMBER         Parallel build jobs (default: 8)" \
		"  --verify-only         Verify an existing prefix" \
		"  -h, --help            Show this help" \
		"" \
		"The build performs no network access. Run dependencies/prepare.sh" \
		"--product ios-gpu first to materialize the pinned commits and patches."
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
[ "$(uname -s)" = Darwin ] || fail "the graphics build requires macOS"
case "$(uname -m)" in
arm64|aarch64) ;;
*) fail "the iOS graphics build requires an arm64 host" ;;
esac
case "$PLATFORM" in
ios-simulator)
	SDK_NAME=iphonesimulator
	TARGET=arm64-apple-ios${DEPLOYMENT_TARGET}-simulator
	SUBSYSTEM=ios-simulator
		RECEIPT_NAME=ios-simulator-gpu-arm64.tsv
		DEFAULT_PREFIX="$ROOT_DIR/.build/dependencies/ios-simulator-arm64/gpu"
	;;
ios)
	SDK_NAME=iphoneos
	TARGET=arm64-apple-ios${DEPLOYMENT_TARGET}
	SUBSYSTEM=ios
		RECEIPT_NAME=ios-device-gpu-arm64.tsv
		DEFAULT_PREFIX="$ROOT_DIR/.build/dependencies/ios-device-arm64/gpu"
	;;
*) fail "platform must be ios or ios-simulator" ;;
esac
PLATFORM_RECEIPT="$ROOT_DIR/dependencies/platforms/$RECEIPT_NAME"
if [ -z "$PREFIX" ]; then
	PREFIX=$DEFAULT_PREFIX
fi
[ -r "$MANIFEST" ] || fail "source manifest is unavailable: $MANIFEST"
[ -r "$PLATFORM_RECEIPT" ] || fail "platform receipt is unavailable: $PLATFORM_RECEIPT"

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
	printf '%s\n' "verified iOS GPU dependency prefix: $PREFIX"
	exit 0
fi

if [ -e "$PREFIX" ] || [ -L "$PREFIX" ]; then
	verify_prefix "$PREFIX" ||
		fail "existing prefix is invalid and was left untouched: $PREFIX"
	printf '%s\n' "reusing verified iOS GPU dependency prefix: $PREFIX"
	exit 0
fi

"$ROOT_DIR/dependencies/prepare.sh" --product ios-gpu \
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
STAGE=$(mktemp -d "$PREFIX_PARENT/.ios-gpu-prefix.XXXXXX")
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

SOURCES="$TEMP_ROOT/sources"
BUILDS="$TEMP_ROOT/builds"
ANGLE_ROOT="$SOURCES/WebKit/Source/ThirdParty/ANGLE"
ANGLE_ARCHIVE="$BUILDS/ANGLE"
IOS_CROSS="$BUILDS/meson-$PLATFORM.cross"
DARWIN_CROSS="$BUILDS/meson-darwin-$PLATFORM.cross"
mkdir -p -- "$ANGLE_ROOT" "$SOURCES/WebKit/Configurations" \
	"$SOURCES/WebKit/Tools/ccache" "$SOURCES/libepoxy" \
	"$SOURCES/virglrenderer" "$BUILDS" "$STAGE/include" \
	"$STAGE/lib" "$STAGE/Frameworks" "$STAGE/share/kernarium"

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

KERNARIUM_TEMP_DIR=${TMPDIR:-/tmp}
(
	cd "$ANGLE_ROOT"
	env -i PATH="$PATH" TMPDIR="$KERNARIUM_TEMP_DIR" \
		"$XCODEBUILD" archive \
		-quiet \
		-archivePath "$ANGLE_ARCHIVE" \
		-scheme ANGLE \
		-sdk "$SDK_NAME" \
		-arch arm64 \
		-configuration Release \
		WEBCORE_LIBRARY_DIR=/usr/local/lib \
		NORMAL_UMBRELLA_FRAMEWORKS_DIR= \
		CODE_SIGNING_ALLOWED=NO \
		IPHONEOS_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
		GCC_TREAT_WARNINGS_AS_ERRORS=NO \
		OTHER_CPLUSPLUSFLAGS='$(inherited) -Wno-error=unnecessary-virtual-specifier'
)
[ -f "$ANGLE_ARCHIVE.xcarchive/Products/usr/local/lib/libEGL.dylib" ] ||
	fail "ANGLE archive did not produce libEGL.dylib"
[ -f "$ANGLE_ARCHIVE.xcarchive/Products/usr/local/lib/libGLESv2.dylib" ] ||
	fail "ANGLE archive did not produce libGLESv2.dylib"
$RSYNC -a "$ANGLE_ROOT/include/" "$STAGE/include/"

cat > "$IOS_CROSS" <<EOF
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
system = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
EOF

sed "s/system = 'ios'/system = 'darwin'\nkernel = 'xnu'\nsubsystem = '$SUBSYSTEM'/" \
	"$IOS_CROSS" > "$DARWIN_CROSS"

PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$STAGE/lib/pkgconfig" \
	PKG_CONFIG_SYSROOT_DIR= \
	"$MESON" setup "$BUILDS/libepoxy" "$SOURCES/libepoxy" \
	--prefix "$STAGE" --buildtype release --cross-file "$IOS_CROSS" \
	-Ddefault_library=static -Dtests=false -Dglx=no -Degl=yes -Dx11=false
"$MESON" compile -C "$BUILDS/libepoxy" -j "$JOBS"
"$MESON" install -C "$BUILDS/libepoxy"

PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$STAGE/lib/pkgconfig" \
	PKG_CONFIG_SYSROOT_DIR= \
	"$MESON" setup "$BUILDS/virglrenderer" "$SOURCES/virglrenderer" \
	--prefix "$STAGE" --buildtype release --cross-file "$DARWIN_CROSS" \
	-Ddefault_library=static -Dtests=false -Dvtest=false \
	-Dcheck-gl-errors=false -Dvenus=false -Dneptune=false \
	-Dplatforms=egl -Drender-server-mode=thread
"$MESON" compile -C "$BUILDS/virglrenderer" -j "$JOBS"
"$MESON" install -C "$BUILDS/virglrenderer"
install -m 0644 "$SOURCES/virglrenderer/src/virgl_hw.h" \
	"$STAGE/include/virgl/virgl_hw.h"

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
	<key>MinimumOSVersion</key><string>$DEPLOYMENT_TARGET</string>
</dict>
</plist>
EOF
	"$PLUTIL" -lint "$framework_dir/Info.plist" >/dev/null
}

install_framework EGL \
	"$ANGLE_ARCHIVE.xcarchive/Products/usr/local/lib/libEGL.dylib" \
	com.kernarium.EGL
install_framework GLESv2 \
	"$ANGLE_ARCHIVE.xcarchive/Products/usr/local/lib/libGLESv2.dylib" \
	com.kernarium.GLESv2

mkdir -p "$STAGE/share/uml-dependencies/patches"
cp "$MANIFEST" "$STAGE/share/uml-dependencies/manifest.tsv"
cp -R "$ROOT_DIR/dependencies/patches/utm-webkit" \
	"$STAGE/share/uml-dependencies/patches/"
cp -R "$ROOT_DIR/dependencies/patches/utm-virglrenderer" \
	"$STAGE/share/uml-dependencies/patches/"
cp "$PLATFORM_RECEIPT" \
	"$STAGE/share/uml-dependencies/$RECEIPT_NAME"
verify_prefix "$STAGE"
mv "$STAGE" "$PREFIX"
STAGE=
printf '%s\n' "published iOS GPU dependency prefix: $PREFIX"
