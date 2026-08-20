#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
REGENERATOR="$ROOT_DIR/dependencies/rebase-patches.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-dependency-rebase.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

fail()
{
	printf '%s\n' "test-rebase-patches.sh: $*" >&2
	exit 1
}

SOURCE="$TEST_ROOT/source"
METADATA="$TEST_ROOT/dependencies"
MANIFEST="$METADATA/manifest.tsv"
mkdir -p "$SOURCE" "$METADATA/patches/fixture"
git -C "$SOURCE" init --quiet
git -C "$SOURCE" config user.name fixture
git -C "$SOURCE" config user.email fixture@example.invalid
printf '%s\n' base >"$SOURCE/payload.txt"
git -C "$SOURCE" add payload.txt
git -C "$SOURCE" commit --quiet -m 'fixture: upstream base'
BASE=$(git -C "$SOURCE" rev-parse HEAD)
git -C "$SOURCE" switch --quiet -c maintained
printf '%s\n' maintained >"$SOURCE/payload.txt"
git -C "$SOURCE" commit --quiet -am 'fixture: maintained change'

cat >"$MANIFEST" <<EOF
name	version	products	repository	commit	source_paths	patch_series
fixture	1.0	fixture	https://example.invalid/fixture.git	$BASE	.	patches/fixture/series.tsv
EOF
UML_DEPENDENCY_MANIFEST="$MANIFEST" \
UML_DEPENDENCY_METADATA_ROOT="$METADATA" \
	"$REGENERATOR" --dependency fixture --source "$SOURCE" >/dev/null

SERIES="$METADATA/patches/fixture/series.tsv"
PATCH=$(awk -F '\t' '$1 == 1 { print $2 }' "$SERIES")
[ -f "$METADATA/patches/fixture/$PATCH" ] ||
	fail "regenerator did not emit the listed patch"
grep -Eq '^From [0-9a-f]{40} Mon Sep 17 00:00:00 2001$' \
	"$METADATA/patches/fixture/$PATCH" ||
	fail "regenerator output is not git format-patch mail"
EXPECTED_ID=$(git -C "$SOURCE" show --pretty=format: HEAD |
	git patch-id --stable | awk '{print $1}')
ACTUAL_ID=$(awk -F '\t' '$1 == 1 { print $3 }' "$SERIES")
[ "$ACTUAL_ID" = "$EXPECTED_ID" ] ||
	fail "regenerator recorded the wrong stable patch ID"

# Modification happens in Git history. Regeneration must follow the amended
# commit and must not retain manually edited bytes.
printf '%s\n' second >>"$SOURCE/payload.txt"
git -C "$SOURCE" commit --quiet --amend -am 'fixture: amended maintained change'
printf '%s\n' '# forbidden manual edit' >>"$METADATA/patches/fixture/$PATCH"
UML_DEPENDENCY_MANIFEST="$MANIFEST" \
UML_DEPENDENCY_METADATA_ROOT="$METADATA" \
	"$REGENERATOR" --dependency fixture --source "$SOURCE" >/dev/null
! grep -R -q 'forbidden manual edit' "$METADATA/patches/fixture" ||
	fail "regenerator retained manual patch-file editing"
grep -q 'amended maintained change' "$METADATA/patches/fixture/series.tsv" ||
	fail "regenerator did not follow amended Git history"

printf '%s\n' dirty >>"$SOURCE/payload.txt"
if UML_DEPENDENCY_MANIFEST="$MANIFEST" \
	UML_DEPENDENCY_METADATA_ROOT="$METADATA" \
	"$REGENERATOR" --dependency fixture --source "$SOURCE" \
	>"$TEST_ROOT/dirty.out" 2>"$TEST_ROOT/dirty.err"; then
	fail "regenerator accepted a dirty source repository"
fi
[ -s "$TEST_ROOT/dirty.err" ] || fail "dirty rejection lacked a diagnostic"

printf '%s\n' "dependency patch regeneration tests passed"
