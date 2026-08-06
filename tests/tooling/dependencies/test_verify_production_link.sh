#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-production-link.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

BUILD_ROOT="$TEST_ROOT/build"
PREFIX="$TEST_ROOT/prefix"
AMBIENT_ROOT="$TEST_ROOT/ambient"
LINK_MAP="$TEST_ROOT/link-map.txt"
BINARY=/usr/bin/true
SDK_ROOT=$(xcrun --sdk macosx --show-sdk-path)

mkdir -p -- "$BUILD_ROOT/objects" "$PREFIX/lib" "$AMBIENT_ROOT"
: >"$BUILD_ROOT/objects/owned.o"
: >"$PREFIX/lib/libz.a"
: >"$AMBIENT_ROOT/libambient.a"

write_link_map()
{
	ambient_object=$1
	{
		printf '%s\n' '# Object files:'
		printf '%s\n' '[ 0] linker synthesized'
		printf '[ 1] %s\n' "$BUILD_ROOT/objects/owned.o"
		printf '[ 2] %s\n' "$PREFIX/lib/libz.a(member.o)"
		printf '[ 3] %s\n' "$SDK_ROOT/usr/lib/libSystem.tbd"
		[ -z "$ambient_object" ] || printf '[ 4] %s\n' "$ambient_object"
		printf '%s\n' '# Sections:'
	} >"$LINK_MAP"
}

verify()
{
	"$ROOT_DIR/dependencies/verify-production-link.sh" \
		--binary "$BINARY" \
		--link-map "$LINK_MAP" \
		--prefix "$PREFIX" \
		--build-root "$BUILD_ROOT"
}

write_link_map ''
verify >/dev/null

: >"$LINK_MAP"
if verify >"$TEST_ROOT/empty.stdout" 2>"$TEST_ROOT/empty.stderr"; then
	printf '%s\n' 'empty link map was accepted unexpectedly' >&2
	exit 1
fi
grep -F 'no unique nonempty object inventory' \
	"$TEST_ROOT/empty.stderr" >/dev/null || {
	printf '%s\n' 'empty link-map rejection lacked the expected diagnostic' >&2
	exit 1
}

{
	printf '%s\n' '# Object files:'
	printf '%s\n' '[ 0] linker synthesized'
	printf '%s\n' '# Sections:'
} >"$LINK_MAP"
if verify >"$TEST_ROOT/synthesized.stdout" \
	2>"$TEST_ROOT/synthesized.stderr"; then
	printf '%s\n' 'link map without a real object was accepted unexpectedly' >&2
	exit 1
fi
grep -F 'no unique nonempty object inventory' \
	"$TEST_ROOT/synthesized.stderr" >/dev/null || {
	printf '%s\n' 'empty object inventory rejection lacked the expected diagnostic' >&2
	exit 1
}

write_link_map ''
{
	printf '%s\n' '# Object files:'
	printf '[ 4] %s\n' "$BUILD_ROOT/objects/owned.o"
	printf '%s\n' '# Sections:'
} >>"$LINK_MAP"
if verify >"$TEST_ROOT/duplicate.stdout" \
	2>"$TEST_ROOT/duplicate.stderr"; then
	printf '%s\n' 'duplicate link-map object inventory was accepted unexpectedly' >&2
	exit 1
fi
grep -F 'no unique nonempty object inventory' \
	"$TEST_ROOT/duplicate.stderr" >/dev/null || {
	printf '%s\n' 'duplicate object inventory rejection lacked the expected diagnostic' >&2
	exit 1
}

write_link_map "$AMBIENT_ROOT/libambient.a(member.o)"
if verify >"$TEST_ROOT/ambient.stdout" 2>"$TEST_ROOT/ambient.stderr"; then
	printf '%s\n' 'ambient link-map input was accepted unexpectedly' >&2
	exit 1
fi
grep -F 'outside the allowed roots' "$TEST_ROOT/ambient.stderr" >/dev/null || {
	printf '%s\n' 'ambient link-map rejection lacked the expected diagnostic' >&2
	exit 1
}

printf '%s\n' 'production link allowlist fixture passed'
