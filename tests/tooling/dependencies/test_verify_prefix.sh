#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

DEPENDENCY_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../dependencies" && pwd)
VERIFIER="$DEPENDENCY_DIR/verify-prefix.sh"
ARCHIVE_TOOL="$DEPENDENCY_DIR/source_archive.py"
DEPLOYMENT_TARGET=14.0
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-dependency-prefix.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

fail()
{
	printf '%s\n' "test-verify-prefix.sh: $*" >&2
	exit 1
}

sha256_file()
{
	shasum -a 256 "$1" | awk '{print $1}'
}

verify()
{
	"$VERIFIER" \
		--prefix "$1" \
		--manifest "$MANIFEST" \
		--build-script "$BUILD_SCRIPT" \
		--deployment-target "$DEPLOYMENT_TARGET"
}

payload_hash()
{
	"$ARCHIVE_TOOL" tree-hash --root "$1" \
		--exclude share/uml-dependencies/receipt.tsv
}

refresh_payload_hash()
{
	prefix=$1
	payload=$(payload_hash "$prefix")
	awk -F '\t' -v payload="$payload" 'BEGIN { OFS = FS }
		$1 == "payload_tree_sha256" { $2 = payload }
		{ print }
	' "$prefix/share/uml-dependencies/receipt.tsv" \
		>"$prefix/share/uml-dependencies/receipt.tsv.changed"
	mv "$prefix/share/uml-dependencies/receipt.tsv.changed" \
		"$prefix/share/uml-dependencies/receipt.tsv"
}

copy_fixture()
{
	destination=$1
	cp -R "$BASE_PREFIX" "$destination"
}

expect_failure()
{
	description=$1
	prefix=$2
	FAILURE_NUMBER=$((FAILURE_NUMBER + 1))
	if verify "$prefix" >"$TEST_ROOT/rejected-$FAILURE_NUMBER.out" \
		2>"$TEST_ROOT/rejected-$FAILURE_NUMBER.err"; then
		fail "$description was incorrectly accepted"
	fi
	[ -s "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" ] ||
		fail "$description was rejected without a diagnostic"
}

command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
MANIFEST="$TEST_ROOT/manifest.tsv"
BUILD_SCRIPT="$TEST_ROOT/build.sh"
BASE_PREFIX="$TEST_ROOT/prefix"
FAILURE_NUMBER=0
mkdir -p "$BASE_PREFIX/include/glib-2.0" \
	"$BASE_PREFIX/include/slirp" \
	"$BASE_PREFIX/include/epoxy" \
	"$BASE_PREFIX/include/rfb" \
	"$BASE_PREFIX/include/virgl" \
	"$BASE_PREFIX/lib/glib-2.0/include" \
	"$BASE_PREFIX/share/uml-dependencies"
printf '%s\n' fixture >"$MANIFEST"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$BUILD_SCRIPT"
cp "$MANIFEST" "$MANIFEST.original"

required_headers='include/zlib.h
include/zconf.h
include/pcre2.h
include/ffi.h
include/iconv.h
include/libintl.h
include/glib-2.0/glib.h
lib/glib-2.0/include/glibconfig.h
include/slirp/libslirp.h
include/epoxy/gl.h
include/rfb/rfb.h
include/rfb/rfbclient.h
include/virgl/virglrenderer.h
include/virgl/virgl_hw.h'
printf '%s\n' "$required_headers" | while IFS= read -r relative; do
	printf '%s\n' fixture >"$BASE_PREFIX/$relative"
done

printf '%s\n' 'int uml_dependency_fixture(void) { return 0; }' \
	>"$TEST_ROOT/fixture.c"
xcrun --sdk macosx clang -arch arm64 \
	-mmacosx-version-min="$DEPLOYMENT_TARGET" -c "$TEST_ROOT/fixture.c" \
	-o "$TEST_ROOT/fixture-arm64.o"
xcrun ar rcs "$TEST_ROOT/fixture-arm64.a" "$TEST_ROOT/fixture-arm64.o"

required_archives='libz.a
libpcre2-8.a
libffi.a
libcharset.a
libiconv.a
libintl.a
libglib-2.0.a
libslirp.a
libepoxy.a
libvncserver.a
libvncclient.a
libvirglrenderer.a'
printf '%s\n' "$required_archives" | while IFS= read -r archive; do
	cp "$TEST_ROOT/fixture-arm64.a" "$BASE_PREFIX/lib/$archive"
done

MANIFEST_HASH=$(sha256_file "$MANIFEST")
BUILD_POLICY_HASH=$(sha256_file "$BUILD_SCRIPT")
ARCHIVE_TOOL_HASH=$(sha256_file "$ARCHIVE_TOOL")

# GLib installs debugger helpers whose filenames embed an ELF shared-library
# basename but end in -gdb.py. They are scripts, not dynamic libraries.
mkdir -p "$BASE_PREFIX/share/gdb/auto-load/private/prefix/lib"
printf '%s\n' fixture \
	>"$BASE_PREFIX/share/gdb/auto-load/private/prefix/lib/libglib-2.0.so.0-gdb.py"

PAYLOAD_HASH=$(payload_hash "$BASE_PREFIX")
cat >"$BASE_PREFIX/share/uml-dependencies/receipt.tsv" <<EOF
schema	2
manifest_sha256	$MANIFEST_HASH
build_policy_sha256	$BUILD_POLICY_HASH
archive_tool_sha256	$ARCHIVE_TOOL_HASH
payload_tree_sha256	$PAYLOAD_HASH
platform	macos
architecture	arm64
linkage	static
deployment_target	$DEPLOYMENT_TARGET
sdk_version	99.0
built_at_utc	2026-08-03T00:00:00Z
compiler	fixture clang
EOF

verify "$BASE_PREFIX" >/dev/null

CASE_PREFIX="$TEST_ROOT/missing-file"
copy_fixture "$CASE_PREFIX"
rm "$CASE_PREFIX/include/zlib.h"
expect_failure "missing required file" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/dynamic-library"
copy_fixture "$CASE_PREFIX"
printf '%s\n' forbidden >"$CASE_PREFIX/lib/libforbidden.dylib"
expect_failure "dynamic library" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/disguised-dynamic-library"
copy_fixture "$CASE_PREFIX"
xcrun --sdk macosx clang -arch arm64 -dynamiclib "$TEST_ROOT/fixture.c" \
	-o "$CASE_PREFIX/lib/hidden.bundle"
refresh_payload_hash "$CASE_PREFIX"
expect_failure "dynamic library with a nonstandard suffix" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/nested-invalid-archive"
copy_fixture "$CASE_PREFIX"
mkdir -p "$CASE_PREFIX/lib/plugins"
printf '%s\n' 'not an archive' >"$CASE_PREFIX/lib/plugins/hidden.a"
refresh_payload_hash "$CASE_PREFIX"
expect_failure "nested invalid static archive" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/cmake-package-metadata"
copy_fixture "$CASE_PREFIX"
mkdir -p "$CASE_PREFIX/lib/cmake/Fixture"
printf '%s\n' '# unused export' \
	>"$CASE_PREFIX/lib/cmake/Fixture/FixtureTargets.cmake"
expect_failure "installed CMake package metadata" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/duplicate-receipt"
copy_fixture "$CASE_PREFIX"
printf '%s\n' 'schema	1' \
	>>"$CASE_PREFIX/share/uml-dependencies/receipt.tsv"
expect_failure "duplicate receipt field" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/missing-sdk-observation"
copy_fixture "$CASE_PREFIX"
sed '/^sdk_version	/d' "$CASE_PREFIX/share/uml-dependencies/receipt.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/receipt.tsv.changed"
mv "$CASE_PREFIX/share/uml-dependencies/receipt.tsv.changed" \
	"$CASE_PREFIX/share/uml-dependencies/receipt.tsv"
expect_failure "missing SDK observation" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/wrong-receipt"
copy_fixture "$CASE_PREFIX"
sed 's/^manifest_sha256.*/manifest_sha256\tffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff/' \
	"$CASE_PREFIX/share/uml-dependencies/receipt.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/receipt.tsv.changed"
mv "$CASE_PREFIX/share/uml-dependencies/receipt.tsv.changed" \
	"$CASE_PREFIX/share/uml-dependencies/receipt.tsv"
expect_failure "wrong receipt provenance" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/manifest-tamper"
copy_fixture "$CASE_PREFIX"
printf '%s\n' changed >>"$MANIFEST"
expect_failure "changed dependency manifest" "$CASE_PREFIX"
cp "$MANIFEST.original" "$MANIFEST"

CASE_PREFIX="$TEST_ROOT/wrong-architecture"
copy_fixture "$CASE_PREFIX"
xcrun --sdk macosx clang -arch x86_64 -c "$TEST_ROOT/fixture.c" \
	-o "$TEST_ROOT/fixture-x86_64.o"
xcrun ar rcs "$TEST_ROOT/fixture-x86_64.a" "$TEST_ROOT/fixture-x86_64.o"
cp "$TEST_ROOT/fixture-x86_64.a" "$CASE_PREFIX/lib/libz.a"
refresh_payload_hash "$CASE_PREFIX"
expect_failure "non-arm64 static archive" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/header-tamper"
copy_fixture "$CASE_PREFIX"
printf '%s\n' changed >>"$CASE_PREFIX/include/zlib.h"
expect_failure "same-path header tampering" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/same-architecture-archive-tamper"
copy_fixture "$CASE_PREFIX"
printf '%s\n' 'int uml_dependency_other(void) { return 1; }' \
	>"$TEST_ROOT/other.c"
xcrun --sdk macosx clang -arch arm64 \
	-mmacosx-version-min="$DEPLOYMENT_TARGET" -c "$TEST_ROOT/other.c" \
	-o "$TEST_ROOT/other-arm64.o"
xcrun ar rcs "$TEST_ROOT/other-arm64.a" "$TEST_ROOT/other-arm64.o"
cp "$TEST_ROOT/other-arm64.a" "$CASE_PREFIX/lib/libz.a"
expect_failure "same-architecture archive tampering" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/wrong-member-deployment-target"
copy_fixture "$CASE_PREFIX"
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=15.0 \
	-c "$TEST_ROOT/fixture.c" -o "$TEST_ROOT/fixture-macos15.o"
xcrun ar rcs "$TEST_ROOT/fixture-macos15.a" "$TEST_ROOT/fixture-macos15.o"
cp "$TEST_ROOT/fixture-macos15.a" "$CASE_PREFIX/lib/libz.a"
refresh_payload_hash "$CASE_PREFIX"
expect_failure "archive member deployment target" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/wrong-member-platform"
copy_fixture "$CASE_PREFIX"
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=15.0 \
	-c "$TEST_ROOT/fixture.c" -o "$TEST_ROOT/fixture-ios.o"
xcrun ar rcs "$TEST_ROOT/fixture-ios.a" "$TEST_ROOT/fixture-ios.o"
cp "$TEST_ROOT/fixture-ios.a" "$CASE_PREFIX/lib/libz.a"
refresh_payload_hash "$CASE_PREFIX"
expect_failure "archive member Apple platform" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/renamed-dynamic-library"
copy_fixture "$CASE_PREFIX"
xcrun --sdk macosx clang -arch arm64 -dynamiclib "$TEST_ROOT/fixture.c" \
	-o "$CASE_PREFIX/lib/libz.a"
refresh_payload_hash "$CASE_PREFIX"
expect_failure "dynamic library renamed as an archive" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/untracked-payload"
copy_fixture "$CASE_PREFIX"
printf '%s\n' unexpected >"$CASE_PREFIX/include/untracked.h"
expect_failure "untracked prefix payload" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/escaping-symlink"
copy_fixture "$CASE_PREFIX"
ln -s ../../../../outside "$CASE_PREFIX/include/escaping-link"
expect_failure "escaping prefix symbolic link" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/staging-path"
copy_fixture "$CASE_PREFIX"
mkdir -p "$CASE_PREFIX/lib/pkgconfig"
printf '%s\n' 'prefix=/tmp/.uml-dependency-image.fixture/private-prefix' \
	>"$CASE_PREFIX/lib/pkgconfig/fixture.pc"
refresh_payload_hash "$CASE_PREFIX"
expect_failure "embedded staging path" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/temporary-build-path"
copy_fixture "$CASE_PREFIX"
mkdir -p "$CASE_PREFIX/lib/pkgconfig"
printf '%s\n' 'source=/tmp/source-build.fixture/sources/dependency' \
	>"$CASE_PREFIX/lib/pkgconfig/fixture.pc"
refresh_payload_hash "$CASE_PREFIX"
expect_failure "embedded temporary build path" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/wrong-deployment-target"
copy_fixture "$CASE_PREFIX"
DEPLOYMENT_TARGET=15.0
expect_failure "incompatible deployment target" "$CASE_PREFIX"
DEPLOYMENT_TARGET=14.0

CASE_PREFIX="$TEST_ROOT/build-policy-tamper"
copy_fixture "$CASE_PREFIX"
printf '%s\n' '# changed' >>"$BUILD_SCRIPT"
expect_failure "changed dependency build policy" "$CASE_PREFIX"

printf '%s\n' "dependency prefix verification tests passed"
