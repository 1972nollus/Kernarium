#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIER="$ROOT_DIR/dependencies/cmake/VerifyIosNetworkPrefix.cmake"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-ios-network-prefix.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test-verify-ios-network-prefix.sh
. "$(dirname -- "$0")/../support/expect.sh"

ARCHIVES="libpcre2-8.a libffi.a libintl.a libglib-2.0.a libslirp.a"

verify()
{
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" -DPLATFORM="$2" \
		-P "$VERIFIER"
}

series_tree()
{
	awk -F '\t' '$1 == "tree" { print $2 }' \
		"$ROOT_DIR/dependencies/patches/$1/series.tsv"
}

# archive OUTPUT SDK TARGET: a one-member static archive for TARGET.
archive()
{
	object="$TEST_ROOT/object-$$-$(basename -- "$1").o"
	xcrun --sdk "$2" clang -target "$3" -c "$TEST_ROOT/fixture.c" -o "$object"
	rm -f -- "$1"
	xcrun ar rcs "$1" "$object"
}

# make_prefix PREFIX PLATFORM: a prefix that passes verification.
make_prefix()
{
	prefix=$1
	case "$2" in
	ios) sdk=iphoneos target=arm64-apple-ios26.0
		receipt=ios-device-network-arm64.tsv ;;
	ios-simulator) sdk=iphonesimulator target=arm64-apple-ios26.0-simulator
		receipt=ios-simulator-network-arm64.tsv ;;
	esac
	mkdir -p "$prefix/lib/glib-2.0/include" "$prefix/include/glib-2.0" \
		"$prefix/include/slirp" "$prefix/share/uml-dependencies"
	for name in $ARCHIVES; do
		archive "$prefix/lib/$name" "$sdk" "$target"
	done
	for header in include/pcre2.h include/ffi.h include/glib-2.0/glib.h \
		lib/glib-2.0/include/glibconfig.h include/slirp/libslirp.h; do
		printf '%s\n' fixture >"$prefix/$header"
	done
	cp "$ROOT_DIR/dependencies/manifest.tsv" \
		"$prefix/share/uml-dependencies/manifest.tsv"
	cp "$ROOT_DIR/dependencies/platforms/$receipt" \
		"$prefix/share/uml-dependencies/$receipt"
	{
		printf 'source\ttree\n'
		for source in glib-ios libslirp; do
			printf '%s\t%s\n' "$source" "$(series_tree "$source")"
		done
	} >"$prefix/share/uml-dependencies/source-trees.tsv"
}

copy_fixture()
{
	cp -R "$DEVICE" "$1"
}

command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
printf '%s\n' 'int uml_ios_network_fixture(void) { return 0; }' \
	>"$TEST_ROOT/fixture.c"
DEVICE="$TEST_ROOT/device"
SIMULATOR="$TEST_ROOT/simulator"
make_prefix "$DEVICE" ios
make_prefix "$SIMULATOR" ios-simulator
TREES="$DEVICE/share/uml-dependencies/source-trees.tsv"

verify "$DEVICE" ios >/dev/null || fail "a valid device prefix was rejected"
verify "$SIMULATOR" ios-simulator >/dev/null ||
	fail "a valid Simulator prefix was rejected"

expect_cmake_diagnostic "Unsupported iOS network platform" \
	verify "$DEVICE" macos
expect_cmake_diagnostic "ios-device-network-arm64.tsv" \
	verify "$SIMULATOR" ios

# Every archive is an arm64 archive for the platform at iOS 26.0.
CASE="$TEST_ROOT/simulator-as-device"
cp -R "$SIMULATOR" "$CASE"
cp "$ROOT_DIR/dependencies/platforms/ios-device-network-arm64.tsv" \
	"$CASE/share/uml-dependencies/"
expect_cmake_diagnostic "lib/libpcre2-8.a" verify "$CASE" ios
CASE="$TEST_ROOT/device-as-simulator"
copy_fixture "$CASE"
cp "$ROOT_DIR/dependencies/platforms/ios-simulator-network-arm64.tsv" \
	"$CASE/share/uml-dependencies/"
expect_cmake_diagnostic "lib/libpcre2-8.a" verify "$CASE" ios-simulator

CASE="$TEST_ROOT/simulator-member"
copy_fixture "$CASE"
archive "$CASE/lib/libslirp.a" iphonesimulator arm64-apple-ios26.0-simulator
expect_cmake_diagnostic "lib/libslirp.a" verify "$CASE" ios

CASE="$TEST_ROOT/old-deployment-target"
copy_fixture "$CASE"
archive "$CASE/lib/libglib-2.0.a" iphoneos arm64-apple-ios18.0
expect_cmake_diagnostic "lib/libglib-2.0.a" verify "$CASE" ios

CASE="$TEST_ROOT/intel-archive"
copy_fixture "$CASE"
archive "$CASE/lib/libffi.a" iphonesimulator x86_64-apple-ios26.0-simulator
expect_cmake_diagnostic "lib/libffi.a" verify "$CASE" ios

CASE="$TEST_ROOT/universal-archive"
copy_fixture "$CASE"
archive "$TEST_ROOT/intel.a" iphonesimulator x86_64-apple-ios26.0-simulator
xcrun lipo -create "$DEVICE/lib/libintl.a" "$TEST_ROOT/intel.a" \
	-output "$CASE/lib/libintl.a"
expect_cmake_diagnostic "lib/libintl.a" verify "$CASE" ios

# Required payload is present and not a symlink.
CASE="$TEST_ROOT/missing-archive"
copy_fixture "$CASE"
rm "$CASE/lib/libpcre2-8.a"
expect_cmake_diagnostic "lib/libpcre2-8.a" verify "$CASE" ios

CASE="$TEST_ROOT/linked-header"
copy_fixture "$CASE"
rm "$CASE/include/slirp/libslirp.h"
ln -s "$DEVICE/include/slirp/libslirp.h" "$CASE/include/slirp/libslirp.h"
expect_cmake_diagnostic "include/slirp/libslirp.h" verify "$CASE" ios

# The installed platform receipt is the checked-in one, byte for byte.
CASE="$TEST_ROOT/edited-receipt"
copy_fixture "$CASE"
sed 's/^format\t2$/format\t3/' \
	"$DEVICE/share/uml-dependencies/ios-device-network-arm64.tsv" \
	>"$CASE/share/uml-dependencies/ios-device-network-arm64.tsv"
cmp -s "$DEVICE/share/uml-dependencies/ios-device-network-arm64.tsv" \
	"$CASE/share/uml-dependencies/ios-device-network-arm64.tsv" &&
	fail "the receipt fixture edit changed nothing"
expect_cmake_diagnostic "provenance does not match" verify "$CASE" ios

CASE="$TEST_ROOT/missing-receipt"
copy_fixture "$CASE"
rm "$CASE/share/uml-dependencies/ios-device-network-arm64.tsv"
expect_cmake_diagnostic "share/uml-dependencies/ios-device-network-arm64.tsv" \
	verify "$CASE" ios

# The prefix was built from the product's manifest rows.
CASE="$TEST_ROOT/changed-manifest-row"
copy_fixture "$CASE"
awk -F '\t' -v OFS='\t' '$1 == "libslirp" { $5 = "0000000" } { print }' \
	"$ROOT_DIR/dependencies/manifest.tsv" \
	>"$CASE/share/uml-dependencies/manifest.tsv"
expect_cmake_diagnostic "manifest rows do not match" verify "$CASE" ios

# Each patched source the manifest names records the tree its series records.
for source in glib-ios libslirp; do
	CASE="$TEST_ROOT/stale-$source"
	copy_fixture "$CASE"
	awk -F '\t' -v OFS='\t' -v source="$source" \
		'$1 == source { $2 = "0000000000000000000000000000000000000000" }
		{ print }' "$TREES" >"$CASE/share/uml-dependencies/source-trees.tsv"
	expect_cmake_diagnostic "$source was built from tree" verify "$CASE" ios

	CASE="$TEST_ROOT/missing-$source"
	copy_fixture "$CASE"
	grep -v "^$source	" "$TREES" \
		>"$CASE/share/uml-dependencies/source-trees.tsv"
	expect_cmake_diagnostic "$source was built from tree ''" \
		verify "$CASE" ios
done

CASE="$TEST_ROOT/extra-source-tree"
copy_fixture "$CASE"
printf 'glib\t%s\n' 0000000000000000000000000000000000000000 \
	>>"$CASE/share/uml-dependencies/source-trees.tsv"
expect_cmake_diagnostic "source trees list" verify "$CASE" ios

CASE="$TEST_ROOT/missing-source-trees"
copy_fixture "$CASE"
rm "$CASE/share/uml-dependencies/source-trees.tsv"
expect_cmake_diagnostic "source trees are missing" verify "$CASE" ios

printf '%s\n' "iOS network prefix verifier tests passed"
