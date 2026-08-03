#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD="$SCRIPT_DIR/build.sh"
SOURCE_MANIFEST="$SCRIPT_DIR/manifest.tsv"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-dependency-recipes.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

fail()
{
	printf '%s\n' "test-build-manifest.sh: $*" >&2
	exit 1
}

check_manifest()
{
	UML_DEPENDENCY_MANIFEST="$1" "$BUILD" --check-manifest-only
}

expect_failure()
{
	description=$1
	manifest=$2
	if check_manifest "$manifest" >"$manifest.out" 2>"$manifest.err"; then
		fail "$description was incorrectly accepted"
	fi
	[ -s "$manifest.err" ] || fail "$description had no diagnostic"
}

check_manifest "$SOURCE_MANIFEST" >/dev/null

MISSING="$TEST_ROOT/missing.tsv"
awk -F '\t' '$1 != "zlib" { print }' "$SOURCE_MANIFEST" >"$MISSING"
expect_failure "missing build recipe" "$MISSING"

EXTRA="$TEST_ROOT/extra.tsv"
awk -F '\t' 'BEGIN { OFS = FS }
	{ print }
	$1 == "zlib" { $1 = "unexpected"; print }
' "$SOURCE_MANIFEST" >"$EXTRA"
expect_failure "extra manifest recipe" "$EXTRA"

REORDERED="$TEST_ROOT/reordered.tsv"
awk -F '\t' '
	$1 == "zlib" { saved = $0; next }
	{ print }
	END { print saved }
' "$SOURCE_MANIFEST" >"$REORDERED"
expect_failure "reordered manifest recipes" "$REORDERED"

VERSION="$TEST_ROOT/version.tsv"
awk -F '\t' 'BEGIN { OFS = FS }
	$1 == "zlib" { $2 = "1.3.1" }
	{ print }
' "$SOURCE_MANIFEST" >"$VERSION"
expect_failure "recipe version drift" "$VERSION"

MALFORMED="$TEST_ROOT/malformed.tsv"
awk -F '\t' 'BEGIN { OFS = FS }
	$1 == "zlib" { NF = 9 }
	{ print }
' "$SOURCE_MANIFEST" >"$MALFORMED"
expect_failure "malformed manifest row" "$MALFORMED"

printf '%s\n' "dependency build recipe manifest tests passed"
