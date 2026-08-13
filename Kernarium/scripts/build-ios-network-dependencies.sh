#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the authenticated GLib/libslirp stack for arm64 iOS.
# The script is offline: it accepts only the already verified dependency
# sources maintained by dependencies/prepare.sh.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SOURCE_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
PREFIX=${KERNARIUM_IOS_NETWORK_PREFIX:-}
WORK_ROOT=${KERNARIUM_IOS_NETWORK_BUILD_ROOT:-"$ROOT_DIR/.build/kernarium-network-dependencies/build-from-source"}
GLIB_PATCH="$ROOT_DIR/Kernarium/dependencies/patches/glib/0001-ios-avoid-unavailable-process-apis.patch"
VERIFY_SCRIPT="$ROOT_DIR/Kernarium/Runtime/cmake/VerifyIosNetworkPrefix.cmake"
JOBS=${KERNARIUM_IOS_NETWORK_JOBS:-8}
DEPLOYMENT_TARGET=16.0
PLATFORM=ios-simulator
VERIFY_ONLY=0

fail()
{
	printf '%s\n' "Kernarium/scripts/build-ios-network-dependencies.sh: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"Usage: Kernarium/scripts/build-ios-network-dependencies.sh [options]" \
		"" \
		"Options:" \
		"  --platform NAME    ios-simulator (default) or ios" \
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
[ "$(uname -s)" = Darwin ] || fail "the network build requires macOS"
case "$(uname -m)" in
arm64|aarch64) ;;
*) fail "the iOS network build requires an arm64 host" ;;
esac
case "$PLATFORM" in
ios-simulator)
	SDK_NAME=iphonesimulator
	TARGET=arm64-apple-ios${DEPLOYMENT_TARGET}-simulator
	SUBSYSTEM=ios-simulator
	CMAKE_SYSROOT=iphonesimulator
	RECEIPT_NAME=ios-simulator-network-arm64.tsv
	DEFAULT_PREFIX="$ROOT_DIR/.build/kernarium-network-dependencies/ios-simulator-arm64"
	;;
ios)
	SDK_NAME=iphoneos
	TARGET=arm64-apple-ios${DEPLOYMENT_TARGET}
	SUBSYSTEM=ios
	CMAKE_SYSROOT=iphoneos
	RECEIPT_NAME=ios-device-network-arm64.tsv
	DEFAULT_PREFIX="$ROOT_DIR/.build/kernarium-network-dependencies/ios-device-arm64"
	;;
*) fail "platform must be ios or ios-simulator" ;;
esac
RECEIPT="$ROOT_DIR/Kernarium/dependencies/$RECEIPT_NAME"
if [ -z "$PREFIX" ]; then
	PREFIX=$DEFAULT_PREFIX
fi
[ -r "$RECEIPT" ] || fail "platform receipt is unavailable: $RECEIPT"

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
	printf '%s\n' "verified Kernarium iOS network prefix: $PREFIX"
	exit 0
fi

if [ -e "$PREFIX" ] || [ -L "$PREFIX" ]; then
	verify_prefix "$PREFIX" ||
		fail "existing prefix is invalid and was left untouched: $PREFIX"
	printf '%s\n' "reusing verified Kernarium iOS network prefix: $PREFIX"
	exit 0
fi

"$ROOT_DIR/dependencies/prepare.sh" --root "$SOURCE_ROOT" --verify

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
if [ -x "$ROOT_DIR/.build/gpu-tools-venv/bin/meson" ]; then
	PATH="$ROOT_DIR/.build/gpu-tools-venv/bin:$PATH"
	export PATH
	MESON="$ROOT_DIR/.build/gpu-tools-venv/bin/meson"
else
	find_tool MESON meson
fi
find_tool NINJA ninja
find_tool MAKE gmake make
find_tool PKG_CONFIG pkg-config
find_tool RSYNC rsync
find_tool PATCH patch
find_tool SHASUM shasum
find_tool XCRUN xcrun

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
STAGE=$(mktemp -d "$PREFIX_PARENT/.kernarium-network-prefix.XXXXXX")
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

BUILDS="$TEMP_ROOT/builds"
GLIB_SOURCE="$TEMP_ROOT/glib"
CROSS_FILE="$BUILDS/meson-$PLATFORM.cross"
mkdir -p -- "$BUILDS" "$GLIB_SOURCE/subprojects/proxy-libintl-0.5" \
	"$STAGE/share/kernarium/patches/glib"
$RSYNC -a "$SOURCE_ROOT/glib/" "$GLIB_SOURCE/"
$RSYNC -a "$SOURCE_ROOT/proxy-libintl/" \
	"$GLIB_SOURCE/subprojects/proxy-libintl-0.5/"
EXPECTED_GLIB_PATCH_HASH=$(awk -F '\t' '
	$1 == "glib_patch_sha256" { print $2; found++ }
	END { if (found != 1) exit 2 }
' "$RECEIPT") || fail "cannot read the GLib patch identity"
ACTUAL_GLIB_PATCH_HASH=$($SHASUM -a 256 "$GLIB_PATCH" | awk '{print $1}')
[ "$ACTUAL_GLIB_PATCH_HASH" = "$EXPECTED_GLIB_PATCH_HASH" ] ||
	fail "the GLib iOS patch does not match the platform receipt"
(
	cd "$GLIB_SOURCE"
	"$PATCH" -p1 < "$GLIB_PATCH"
)

printf '%s\n' "building PCRE2 10.47 for $PLATFORM"
"$CMAKE" -S "$SOURCE_ROOT/pcre2" -B "$BUILDS/pcre2" -G Ninja \
	-DCMAKE_MAKE_PROGRAM="$NINJA" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$STAGE" \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DCMAKE_SYSTEM_NAME=iOS \
	-DCMAKE_OSX_SYSROOT="$CMAKE_SYSROOT" \
	-DCMAKE_OSX_ARCHITECTURES=arm64 \
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
	-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
	-DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
	-DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF \
	-DPCRE2_BUILD_PCRE2_32=OFF -DPCRE2_BUILD_PCRE2GREP=OFF \
	-DPCRE2_BUILD_TESTS=OFF -DPCRE2_SUPPORT_LIBZ=OFF \
	-DPCRE2_SUPPORT_LIBBZ2=OFF -DPCRE2_SUPPORT_LIBREADLINE=OFF \
	-DPCRE2_SUPPORT_LIBEDIT=OFF
"$CMAKE" --build "$BUILDS/pcre2" --parallel "$JOBS"
"$CMAKE" --install "$BUILDS/pcre2"

printf '%s\n' "building libffi 3.7.1 for $PLATFORM"
mkdir -p -- "$BUILDS/libffi"
(
	cd "$BUILDS/libffi"
	env CC="$CC -target $TARGET -isysroot $SDK_ROOT" \
		CXX="$CXX -target $TARGET -isysroot $SDK_ROOT" \
		AR="$AR" NM="$NM" RANLIB="$RANLIB" \
		CFLAGS="-O2 -fPIC" LDFLAGS="-target $TARGET -isysroot $SDK_ROOT" \
		"$SOURCE_ROOT/libffi/configure" \
		--build=arm-apple-darwin --host=aarch64-apple-darwin \
		--prefix="$STAGE" --libdir="$STAGE/lib" \
		--disable-shared --enable-static --disable-docs \
		--disable-multi-os-directory
	"$MAKE" -j "$JOBS"
	"$MAKE" install
)

cat > "$CROSS_FILE" <<EOF
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

[host_machine]
system = 'darwin'
kernel = 'xnu'
subsystem = '$SUBSYSTEM'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
EOF

meson_setup()
{
	PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$STAGE/lib/pkgconfig" \
		PKG_CONFIG_SYSROOT_DIR= \
		"$MESON" setup "$@" --cross-file "$CROSS_FILE"
}

printf '%s\n' "building GLib 2.88.3 for $PLATFORM"
meson_setup "$BUILDS/glib" "$GLIB_SOURCE" \
	--prefix "$STAGE" --libdir=lib --buildtype=release \
	--default-library=static --wrap-mode=nodownload \
	--force-fallback-for=intl \
	-Dtests=false -Dinstalled_tests=false -Dnls=disabled \
	-Dintrospection=disabled -Ddocumentation=false -Dman-pages=disabled \
	-Dselinux=disabled -Dlibmount=disabled -Dsysprof=disabled \
	-Dxattr=false -Ddtrace=disabled -Dsystemtap=disabled \
	-Dlibelf=disabled -Dglib_debug=disabled -Dbsymbolic_functions=false
"$MESON" compile -C "$BUILDS/glib" -j "$JOBS"
"$MESON" install -C "$BUILDS/glib"

printf '%s\n' "building libslirp 4.9.3 for $PLATFORM"
meson_setup "$BUILDS/libslirp" "$SOURCE_ROOT/libslirp" \
	--prefix "$STAGE" --libdir=lib --buildtype=release \
	--default-library=static --wrap-mode=nofallback \
	-Dstatic=false -Doss-fuzz=false -Dllvm-fuzz=false \
	-Dfuzz-reproduce=false
"$MESON" compile -C "$BUILDS/libslirp" -j "$JOBS"
"$MESON" install -C "$BUILDS/libslirp"

cp "$ROOT_DIR/dependencies/manifest.tsv" \
	"$STAGE/share/kernarium/dependency-manifest.tsv"
cp "$RECEIPT" \
	"$STAGE/share/kernarium/$RECEIPT_NAME"
cp "$GLIB_PATCH" \
	"$STAGE/share/kernarium/patches/glib/0001-ios-avoid-unavailable-process-apis.patch"
verify_prefix "$STAGE"
mv "$STAGE" "$PREFIX"
STAGE=
printf '%s\n' "published Kernarium iOS network prefix: $PREFIX"
