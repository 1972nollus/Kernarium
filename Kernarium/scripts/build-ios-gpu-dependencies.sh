#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the exact UTM ANGLE/libepoxy/virglrenderer graphics stack used by the
# Kernarium iOS Simulator prototype. This script never clones or downloads;
# callers provide clean local checkouts at the revisions in the manifest.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
MANIFEST="$ROOT_DIR/Kernarium/dependencies/utm-gpu-sources.tsv"
PLATFORM_RECEIPT="$ROOT_DIR/Kernarium/dependencies/ios-simulator-arm64.tsv"
VERIFY_SCRIPT="$ROOT_DIR/Kernarium/Runtime/cmake/VerifyIosGpuPrefix.cmake"
PREFIX=${KERNARIUM_IOS_GPU_PREFIX:-"$ROOT_DIR/.build/kernarium-gpu-dependencies/ios-simulator-arm64"}
WORK_ROOT=${KERNARIUM_IOS_GPU_BUILD_ROOT:-"$ROOT_DIR/.build/kernarium-gpu-dependencies/build-from-source"}
JOBS=${KERNARIUM_IOS_GPU_JOBS:-8}
DEPLOYMENT_TARGET=16.0
WEBKIT_SOURCE=
EPOXY_SOURCE=
VIRGL_SOURCE=
VERIFY_ONLY=0
CHECK_SOURCES_ONLY=0

fail()
{
	printf '%s\n' "Kernarium/scripts/build-ios-gpu-dependencies.sh: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: Kernarium/scripts/build-ios-gpu-dependencies.sh [options]" \
		"" \
		"Options:" \
		"  --webkit PATH        Clean UTM WebKit checkout" \
		"  --libepoxy PATH       Clean UTM libepoxy checkout" \
		"  --virglrenderer PATH  Clean UTM virglrenderer checkout" \
		"  --prefix PATH         Output prefix" \
		"  --work-root PATH      Temporary build parent" \
		"  --jobs NUMBER         Parallel build jobs (default: 8)" \
		"  --verify-only         Verify an existing prefix without sources" \
		"  --check-sources-only  Verify the three source checkouts and stop" \
		"  -h, --help            Show this help" \
		"" \
		"The script performs no network access. It rejects a source checkout" \
		"whose HEAD, origin, or clean-worktree state differs from the manifest."
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--webkit)
		[ "$#" -ge 2 ] || fail "--webkit requires a path"
		WEBKIT_SOURCE=$2
		shift 2
		;;
	--libepoxy)
		[ "$#" -ge 2 ] || fail "--libepoxy requires a path"
		EPOXY_SOURCE=$2
		shift 2
		;;
	--virglrenderer)
		[ "$#" -ge 2 ] || fail "--virglrenderer requires a path"
		VIRGL_SOURCE=$2
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
	--check-sources-only)
		CHECK_SOURCES_ONLY=1
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
*) fail "the iOS Simulator graphics build requires an arm64 host" ;;
esac
[ -r "$MANIFEST" ] || fail "source manifest is unavailable: $MANIFEST"
[ -r "$PLATFORM_RECEIPT" ] || fail "platform receipt is unavailable: $PLATFORM_RECEIPT"

PREFIX_PARENT=$(dirname -- "$PREFIX")
mkdir -p -- "$PREFIX_PARENT"
PREFIX_PARENT=$(CDPATH= cd -- "$PREFIX_PARENT" && pwd -P)
PREFIX="$PREFIX_PARENT/$(basename -- "$PREFIX")"

verify_prefix()
{
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" -P "$VERIFY_SCRIPT"
}

if [ "$VERIFY_ONLY" -eq 1 ]; then
	verify_prefix "$PREFIX"
	printf '%s\n' "verified Kernarium iOS GPU prefix: $PREFIX"
	exit 0
fi

if [ "$CHECK_SOURCES_ONLY" -eq 0 ] &&
   { [ -e "$PREFIX" ] || [ -L "$PREFIX" ]; }; then
	verify_prefix "$PREFIX" ||
		fail "existing prefix is invalid and was left untouched: $PREFIX"
	printf '%s\n' "reusing verified Kernarium iOS GPU prefix: $PREFIX"
	exit 0
fi

[ -n "$WEBKIT_SOURCE" ] || fail "--webkit is required for a new prefix"
[ -n "$EPOXY_SOURCE" ] || fail "--libepoxy is required for a new prefix"
[ -n "$VIRGL_SOURCE" ] || fail "--virglrenderer is required for a new prefix"

manifest_value()
{
	awk -F '\t' -v dependency="$1" -v column="$2" '
		/^#/ || /^[[:space:]]*$/ || $1 == "name" { next }
		$1 == dependency { print $column; found++; }
		END { if (found != 1) exit 2 }
	' "$MANIFEST"
}

verify_source()
{
	dependency=$1
	source_path=$2
	[ -d "$source_path/.git" ] || fail "$dependency is not a Git checkout: $source_path"
	source_path=$(CDPATH= cd -- "$source_path" && pwd -P)
	expected_repository=$(manifest_value "$dependency" 2) ||
		fail "cannot read $dependency repository identity"
	expected_commit=$(manifest_value "$dependency" 3) ||
		fail "cannot read $dependency commit identity"
	actual_repository=$(git -C "$source_path" remote get-url origin 2>/dev/null || true)
	actual_commit=$(git -C "$source_path" rev-parse HEAD 2>/dev/null || true)
	[ "$actual_repository" = "$expected_repository" ] ||
		fail "$dependency origin differs from the pinned repository"
	[ "$actual_commit" = "$expected_commit" ] ||
		fail "$dependency HEAD is $actual_commit, expected $expected_commit"
	[ -z "$(git -C "$source_path" status --porcelain --untracked-files=all)" ] ||
		fail "$dependency checkout is not clean: $source_path"
	required_subtree=$(manifest_value "$dependency" 4) ||
		fail "cannot read $dependency required subtree"
	if [ "$required_subtree" != "." ]; then
		[ -d "$source_path/$required_subtree" ] ||
			fail "$dependency required subtree is missing: $required_subtree"
	fi
	eval "$3=\$source_path"
}

verify_source WebKit "$WEBKIT_SOURCE" WEBKIT_SOURCE
verify_source libepoxy "$EPOXY_SOURCE" EPOXY_SOURCE
verify_source virglrenderer "$VIRGL_SOURCE" VIRGL_SOURCE
if [ "$CHECK_SOURCES_ONLY" -eq 1 ]; then
	printf '%s\n' "verified every pinned Kernarium iOS GPU source"
	exit 0
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
	fail "required build tool is unavailable: $*"
}

if [ -x "$ROOT_DIR/.build/gpu-tools-venv/bin/meson" ]; then
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

SDK_ROOT=$($XCRUN --sdk iphonesimulator --show-sdk-path)
[ -d "$SDK_ROOT" ] || fail "xcrun returned an invalid Simulator SDK"
CC=$($XCRUN --sdk iphonesimulator --find clang)
CXX=$($XCRUN --sdk iphonesimulator --find clang++)
AR=$($XCRUN --sdk iphonesimulator --find ar)
NM=$($XCRUN --sdk iphonesimulator --find nm)
RANLIB=$($XCRUN --sdk iphonesimulator --find ranlib)
STRIP=$($XCRUN --sdk iphonesimulator --find strip)

mkdir -p -- "$WORK_ROOT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
TEMP_ROOT=$(mktemp -d "$WORK_ROOT/source-build.XXXXXX")
STAGE=$(mktemp -d "$PREFIX_PARENT/.kernarium-gpu-prefix.XXXXXX")
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
IOS_CROSS="$BUILDS/meson-ios-simulator.cross"
DARWIN_CROSS="$BUILDS/meson-darwin-ios-simulator.cross"
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
		-archivePath "$ANGLE_ARCHIVE" \
		-scheme ANGLE \
		-sdk iphonesimulator \
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
c_args = ['-arch', 'arm64', '-target', 'arm64-apple-ios${DEPLOYMENT_TARGET}-simulator', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
cpp_args = ['-arch', 'arm64', '-target', 'arm64-apple-ios${DEPLOYMENT_TARGET}-simulator', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
objc_args = ['-arch', 'arm64', '-target', 'arm64-apple-ios${DEPLOYMENT_TARGET}-simulator', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
objcpp_args = ['-arch', 'arm64', '-target', 'arm64-apple-ios${DEPLOYMENT_TARGET}-simulator', '-isysroot', '$SDK_ROOT', '-I$STAGE/include']
c_link_args = ['-arch', 'arm64', '-target', 'arm64-apple-ios${DEPLOYMENT_TARGET}-simulator', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib']
cpp_link_args = ['-arch', 'arm64', '-target', 'arm64-apple-ios${DEPLOYMENT_TARGET}-simulator', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib']
objc_link_args = ['-arch', 'arm64', '-target', 'arm64-apple-ios${DEPLOYMENT_TARGET}-simulator', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib']
objcpp_link_args = ['-arch', 'arm64', '-target', 'arm64-apple-ios${DEPLOYMENT_TARGET}-simulator', '-isysroot', '$SDK_ROOT', '-L$STAGE/lib']

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

sed "s/system = 'ios'/system = 'darwin'\nkernel = 'xnu'\nsubsystem = 'ios-simulator'/" \
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

cp "$MANIFEST" "$STAGE/share/kernarium/utm-gpu-sources.tsv"
cp "$PLATFORM_RECEIPT" \
	"$STAGE/share/kernarium/ios-simulator-arm64.tsv"
verify_prefix "$STAGE"
mv "$STAGE" "$PREFIX"
STAGE=
printf '%s\n' "published Kernarium iOS GPU prefix: $PREFIX"
