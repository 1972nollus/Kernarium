#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREPARE="$SCRIPT_DIR/prepare.sh"
ARCHIVE_TOOL="$SCRIPT_DIR/source_archive.py"
PYTHON=${PYTHON:-python3}
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-dependency-prepare.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

fail()
{
	printf '%s\n' "test-prepare.sh: $*" >&2
	exit 1
}

sha256_file()
{
	shasum -a 256 "$1" | awk '{print $1}'
}

tree_hash()
{
	"$PYTHON" "$ARCHIVE_TOOL" tree-hash --root "$1"
}

prepare()
{
	UML_DEPENDENCY_MANIFEST="$MANIFEST" \
	UML_DEPENDENCY_PATCH_ROOT="$PATCH_ROOT" \
		"$PREPARE" --root "$1" --cache "$CACHE" --no-fetch
}

verify()
{
	UML_DEPENDENCY_MANIFEST="$MANIFEST" \
	UML_DEPENDENCY_PATCH_ROOT="$PATCH_ROOT" \
		"$PREPARE" --root "$1" --cache "$CACHE" --verify
}

expect_failure()
{
	description=$1
	shift
	FAILURE_NUMBER=$((FAILURE_NUMBER + 1))
	if "$@" >"$TEST_ROOT/rejected-$FAILURE_NUMBER.out" \
		2>"$TEST_ROOT/rejected-$FAILURE_NUMBER.err"; then
		fail "$description was incorrectly accepted"
	fi
	[ -s "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" ] ||
		fail "$description was rejected without a diagnostic"
}

CACHE="$TEST_ROOT/cache"
PATCH_ROOT="$TEST_ROOT/patches"
ARCHIVE_INPUT="$TEST_ROOT/archive-input"
REFERENCE="$TEST_ROOT/reference"
PREPARED="$TEST_ROOT/prepared"
MANIFEST="$TEST_ROOT/manifest.tsv"
PATCH="$PATCH_ROOT/fixture.patch"
ARCHIVE="$CACHE/fixture-1.0.tar.gz"
FAILURE_NUMBER=0
mkdir -p "$CACHE" "$PATCH_ROOT" "$REFERENCE" \
	"$ARCHIVE_INPUT/fixture-1.0/sub"
printf '%s\n' baseline >"$ARCHIVE_INPUT/fixture-1.0/payload.txt"
printf '%s\n' secondary >"$ARCHIVE_INPUT/fixture-1.0/sub/secondary.txt"
chmod +x "$ARCHIVE_INPUT/fixture-1.0/sub/secondary.txt"
COPYFILE_DISABLE=1 tar -C "$ARCHIVE_INPUT" -czf "$ARCHIVE" fixture-1.0
cp "$ARCHIVE" "$ARCHIVE.original"

"$PYTHON" "$ARCHIVE_TOOL" extract \
	--archive "$ARCHIVE" \
	--expected-root fixture-1.0 \
	--destination "$REFERENCE/clean"
CLEAN_TREE_HASH=$(tree_hash "$REFERENCE/clean")

cat >"$PATCH" <<'EOF'
--- a/payload.txt
+++ b/payload.txt
@@ -1 +1 @@
-baseline
+patched
EOF
cp -R "$REFERENCE/clean" "$REFERENCE/patched"
patch --batch --forward -p1 -d "$REFERENCE/patched" <"$PATCH" >/dev/null
PATCHED_TREE_HASH=$(tree_hash "$REFERENCE/patched")

ARCHIVE_HASH=$(sha256_file "$ARCHIVE")
PATCH_HASH=$(sha256_file "$PATCH")
cat >"$MANIFEST" <<EOF
name	version	kind	archive_url	archive_file	archive_sha256	archive_root	prepared_tree_sha256	patch	patch_sha256
clean	1.0	release	https://example.invalid/fixture-1.0.tar.gz	fixture-1.0.tar.gz	$ARCHIVE_HASH	fixture-1.0	$CLEAN_TREE_HASH	-	-
patched	1.0	release	https://example.invalid/fixture-1.0.tar.gz	fixture-1.0.tar.gz	$ARCHIVE_HASH	fixture-1.0	$PATCHED_TREE_HASH	fixture.patch	$PATCH_HASH
EOF
cp "$MANIFEST" "$MANIFEST.original"
cp "$PATCH" "$PATCH.original"

prepare "$PREPARED" >/dev/null
verify "$PREPARED" >/dev/null
[ "$(cat "$PREPARED/clean/payload.txt")" = baseline ] ||
	fail "unpatched dependency contents changed"
[ "$(cat "$PREPARED/patched/payload.txt")" = patched ] ||
	fail "dependency patch was not applied"
[ -x "$PREPARED/clean/sub/secondary.txt" ] ||
	fail "prepared tree lost executable status"

CONCURRENT="$TEST_ROOT/concurrent"
prepare "$CONCURRENT" >"$TEST_ROOT/concurrent-1.out" 2>&1 &
FIRST_PREPARER=$!
prepare "$CONCURRENT" >"$TEST_ROOT/concurrent-2.out" 2>&1 &
SECOND_PREPARER=$!
wait "$FIRST_PREPARER" || fail "first concurrent preparer failed"
wait "$SECOND_PREPARER" || fail "second concurrent preparer failed"
verify "$CONCURRENT" >/dev/null
[ ! -e "$CONCURRENT/clean/clean" ] ||
	fail "concurrent publication nested one prepared tree inside another"
[ ! -e "$CONCURRENT/patched/patched" ] ||
	fail "concurrent publication nested one patched tree inside another"

printf '%s\n' tampered >>"$PREPARED/clean/payload.txt"
expect_failure "prepared-tree tampering" verify "$PREPARED"
cp "$REFERENCE/clean/payload.txt" "$PREPARED/clean/payload.txt"
verify "$PREPARED" >/dev/null

printf '%s\n' tampered >>"$ARCHIVE"
expect_failure "cached archive tampering" prepare "$TEST_ROOT/archive-tamper"
cp "$ARCHIVE.original" "$ARCHIVE"

printf '%s\n' '# tampered' >>"$PATCH"
expect_failure "patch tampering" prepare "$TEST_ROOT/patch-tamper"
cp "$PATCH.original" "$PATCH"

tail -n 1 "$MANIFEST.original" >>"$MANIFEST"
expect_failure "duplicate dependency identity" verify "$PREPARED"
cp "$MANIFEST.original" "$MANIFEST"

mkdir "$TEST_ROOT/existing-mismatch"
cp -R "$REFERENCE/clean" "$TEST_ROOT/existing-mismatch/clean"
printf '%s\n' tampered >>"$TEST_ROOT/existing-mismatch/clean/payload.txt"
expect_failure "replacement of an existing mismatched tree" \
	prepare "$TEST_ROOT/existing-mismatch"
[ "$(tail -n 1 "$TEST_ROOT/existing-mismatch/clean/payload.txt")" = tampered ] ||
	fail "failed preparation replaced the mismatched source tree"

mv "$ARCHIVE" "$ARCHIVE.absent"
expect_failure "missing offline archive" prepare "$TEST_ROOT/missing-cache"
mv "$ARCHIVE.absent" "$ARCHIVE"

printf '%s\n' "dependency archive preparation tests passed"
