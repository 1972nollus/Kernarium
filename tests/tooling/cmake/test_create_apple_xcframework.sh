#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

STAGER=${1:?create-apple-xcframework.py is required}
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-apple-xcframework.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM
TEST_ROOT=$(CDPATH= cd -- "$TEST_ROOT" && pwd -P)
failure_index=0

fail()
{
	printf '%s\n' "test_create_apple_xcframework.sh: $*" >&2
	exit 1
}

expect_failure()
{
	pattern=$1
	shift
	failure_index=$((failure_index + 1))
	log="$TEST_ROOT/failure-$failure_index.log"
	if python3 -B "$STAGER" "$@" >"$log" 2>&1; then
		fail "unexpected success: $*"
	fi
	grep -F "$pattern" "$log" >/dev/null || {
		cat "$log" >&2
		fail "failure lacked expected diagnostic: $pattern"
	}
}

make_slice()
{
	platform=$1
	root="$TEST_ROOT/install-$platform"
	mkdir -p -- "$root/lib" "$root/include/uml"
	printf 'archive:%s\n' "$platform" >"$root/lib/libuml_emulator.a"
	printf 'abi:%s\n' "$platform" >"$root/include/uml/abi_identity.h"
	printf 'api:%s\n' "$platform" >"$root/include/uml/emulator.h"
}

for platform in macos ios ios-simulator; do
	make_slice "$platform"
done

OUTPUT="$TEST_ROOT/output/UMLEmulator.xcframework"
python3 -B "$STAGER" \
	--slice "macos=$TEST_ROOT/install-macos" \
	--slice "ios=$TEST_ROOT/install-ios" \
	--slice "ios-simulator=$TEST_ROOT/install-ios-simulator" \
	--output "$OUTPUT"
plutil -lint "$OUTPUT/Info.plist" >/dev/null

python3 -B - "$OUTPUT" <<'PY'
from pathlib import Path
import plistlib
import sys

root = Path(sys.argv[1])
with (root / "Info.plist").open("rb") as stream:
    actual = plistlib.load(stream)
expected = {
    "AvailableLibraries": [
        {
            "BinaryPath": "libuml_emulator.a",
            "HeadersPath": "Headers",
            "LibraryIdentifier": "ios-arm64",
            "LibraryPath": "libuml_emulator.a",
            "SupportedArchitectures": ["arm64"],
            "SupportedPlatform": "ios",
        },
        {
            "BinaryPath": "libuml_emulator.a",
            "HeadersPath": "Headers",
            "LibraryIdentifier": "macos-arm64",
            "LibraryPath": "libuml_emulator.a",
            "SupportedArchitectures": ["arm64"],
            "SupportedPlatform": "macos",
        },
        {
            "BinaryPath": "libuml_emulator.a",
            "HeadersPath": "Headers",
            "LibraryIdentifier": "ios-arm64-simulator",
            "LibraryPath": "libuml_emulator.a",
            "SupportedArchitectures": ["arm64"],
            "SupportedPlatform": "ios",
            "SupportedPlatformVariant": "simulator",
        },
    ],
    "CFBundlePackageType": "XFWK",
    "XCFrameworkFormatVersion": "1.0",
}
if actual != expected:
    raise SystemExit(f"unexpected Info.plist: {actual!r}")
expected_paths = {
    "Info.plist",
    "ios-arm64/Headers/uml/abi_identity.h",
    "ios-arm64/Headers/uml/emulator.h",
    "ios-arm64/libuml_emulator.a",
    "ios-arm64-simulator/Headers/uml/abi_identity.h",
    "ios-arm64-simulator/Headers/uml/emulator.h",
    "ios-arm64-simulator/libuml_emulator.a",
    "macos-arm64/Headers/uml/abi_identity.h",
    "macos-arm64/Headers/uml/emulator.h",
    "macos-arm64/libuml_emulator.a",
}
actual_paths = {
    path.relative_to(root).as_posix()
    for path in root.rglob("*")
    if path.is_file()
}
if actual_paths != expected_paths:
    raise SystemExit(f"unexpected package payload: {sorted(actual_paths)!r}")
PY

for mapping in \
	'ios:ios-arm64' \
	'macos:macos-arm64' \
	'ios-simulator:ios-arm64-simulator'
do
	platform=${mapping%%:*}
	identifier=${mapping#*:}
	cmp "$TEST_ROOT/install-$platform/lib/libuml_emulator.a" \
		"$OUTPUT/$identifier/libuml_emulator.a"
	cmp "$TEST_ROOT/install-$platform/include/uml/abi_identity.h" \
		"$OUTPUT/$identifier/Headers/uml/abi_identity.h"
	cmp "$TEST_ROOT/install-$platform/include/uml/emulator.h" \
		"$OUTPUT/$identifier/Headers/uml/emulator.h"
done

expect_failure 'malformed --slice' \
	--slice "macos=$TEST_ROOT/install-macos" \
	--slice "ios=$TEST_ROOT/install-ios" \
	--slice malformed \
	--output "$TEST_ROOT/malformed/UMLEmulator.xcframework"
expect_failure 'duplicate Apple slice platform: macos' \
	--slice "macos=$TEST_ROOT/install-macos" \
	--slice "macos=$TEST_ROOT/install-macos" \
	--slice "ios=$TEST_ROOT/install-ios" \
	--slice "ios-simulator=$TEST_ROOT/install-ios-simulator" \
	--output "$TEST_ROOT/duplicate/UMLEmulator.xcframework"
expect_failure 'missing required Apple slices: ios-simulator' \
	--slice "macos=$TEST_ROOT/install-macos" \
	--slice "ios=$TEST_ROOT/install-ios" \
	--output "$TEST_ROOT/missing/UMLEmulator.xcframework"
expect_failure 'unsupported Apple slice platform' \
	--slice "macos=$TEST_ROOT/install-macos" \
	--slice "ios=$TEST_ROOT/install-ios" \
	--slice "tvos=$TEST_ROOT/install-ios-simulator" \
	--output "$TEST_ROOT/unknown/UMLEmulator.xcframework"

mkdir -p -- "$TEST_ROOT/symlink-install/lib" "$TEST_ROOT/symlink-install/include/uml"
cp "$TEST_ROOT/install-macos/lib/libuml_emulator.a" \
	"$TEST_ROOT/symlink-install/lib/libuml_emulator.a"
cp "$TEST_ROOT/install-macos/include/uml/abi_identity.h" \
	"$TEST_ROOT/symlink-install/include/uml/abi_identity.h"
ln -s "$TEST_ROOT/install-macos/include/uml/emulator.h" \
	"$TEST_ROOT/symlink-install/include/uml/emulator.h"
expect_failure 'public header tree contains a symlink' \
	--slice "macos=$TEST_ROOT/symlink-install" \
	--slice "ios=$TEST_ROOT/install-ios" \
	--slice "ios-simulator=$TEST_ROOT/install-ios-simulator" \
	--output "$TEST_ROOT/symlink/UMLEmulator.xcframework"

expect_failure 'refusing to replace existing XCFramework output' \
	--slice "macos=$TEST_ROOT/install-macos" \
	--slice "ios=$TEST_ROOT/install-ios" \
	--slice "ios-simulator=$TEST_ROOT/install-ios-simulator" \
	--output "$OUTPUT"

for rejected in malformed duplicate missing unknown symlink; do
	[ ! -e "$TEST_ROOT/$rejected/UMLEmulator.xcframework" ] ||
		fail "rejected $rejected package was published"
done

printf '%s\n' 'Apple XCFramework staging tests passed'
