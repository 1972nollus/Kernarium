#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

DEPENDENCY_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../dependencies" && pwd)
PREPARE="$DEPENDENCY_DIR/prepare.sh"
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

prepare()
{
	UML_DEPENDENCY_MANIFEST="$MANIFEST" \
	UML_DEPENDENCY_PATCH_ROOT="$METADATA" \
		"$PREPARE" --product fixture --root "$1" --cache "$CACHE" --no-fetch
}

verify()
{
	UML_DEPENDENCY_MANIFEST="$MANIFEST" \
	UML_DEPENDENCY_PATCH_ROOT="$METADATA" \
		"$PREPARE" --product fixture --root "$1" --cache "$CACHE" --verify
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
METADATA="$TEST_ROOT/metadata"
UPSTREAM="$TEST_ROOT/upstream"
PREPARED="$TEST_ROOT/prepared"
MANIFEST="$METADATA/manifest.tsv"
SERIES_DIR="$METADATA/patches/patched"
FAILURE_NUMBER=0
mkdir -p "$CACHE/git" "$METADATA/patches" "$UPSTREAM"

git -C "$UPSTREAM" init --quiet
git -C "$UPSTREAM" config user.name fixture
git -C "$UPSTREAM" config user.email fixture@example.invalid
printf '%s\n' baseline >"$UPSTREAM/payload.txt"
mkdir -p "$UPSTREAM/sub"
printf '#!/bin/sh\nexit 0\n' >"$UPSTREAM/sub/tool.sh"
chmod +x "$UPSTREAM/sub/tool.sh"
mkdir -p "$UPSTREAM/other"
printf '%s\n' unselected >"$UPSTREAM/other/unselected.txt"
git -C "$UPSTREAM" add payload.txt sub/tool.sh other/unselected.txt
git -C "$UPSTREAM" commit --quiet -m 'fixture: upstream base'
BASE=$(git -C "$UPSTREAM" rev-parse HEAD)

printf '%s\n' patched >"$UPSTREAM/payload.txt"
git -C "$UPSTREAM" commit --quiet -am 'fixture: first maintained change'
printf '%s\n' added >"$UPSTREAM/added.txt"
git -C "$UPSTREAM" add added.txt
git -C "$UPSTREAM" commit --quiet -m 'fixture: second maintained change'
mkdir -p "$SERIES_DIR"
git -C "$UPSTREAM" format-patch --no-signature --output-directory "$SERIES_DIR" \
	"$BASE..HEAD" >/dev/null
{
	printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later'
	printf 'order\tpatch\tstable_patch_id\tsha256\tsubject\n'
	order=1
	find "$SERIES_DIR" -type f -name '*.patch' -print | LC_ALL=C sort |
	while IFS= read -r patch; do
		patch_id=$(git patch-id --stable <"$patch" | awk '{print $1}')
		digest=$(sha256_file "$patch")
		subject=$(git mailinfo /dev/null /dev/null <"$patch" |
			sed -n 's/^Subject: //p')
		printf '%s\t%s\t%s\t%s\t%s\n' "$order" "$(basename "$patch")" \
			"$patch_id" "$digest" "$subject"
		order=$((order + 1))
	done
} >"$SERIES_DIR/series.tsv"

for name in clean patched sparse; do
	git clone --quiet "$UPSTREAM" "$CACHE/git/$name"
	git -C "$CACHE/git/$name" remote set-url origin \
		"https://example.invalid/$name.git"
	git -C "$CACHE/git/$name" update-ref refs/heads/uml-pin "$BASE"
	git -C "$CACHE/git/$name" config uploadpack.allowFilter true
done

cat >"$MANIFEST" <<EOF
name	version	products	repository	commit	source_paths	patch_series
clean	1.0	fixture	https://example.invalid/clean.git	$BASE	.	-
patched	1.0	fixture	https://example.invalid/patched.git	$BASE	.	patches/patched/series.tsv
sparse	1.0	sparse-fixture	https://example.invalid/sparse.git	$BASE	sub	-
EOF
cp "$MANIFEST" "$MANIFEST.original"
cp -R "$SERIES_DIR" "$TEST_ROOT/series-original"

cp "$(find "$SERIES_DIR" -name '*.patch' -print -quit)" \
	"$METADATA/patches/unlisted.patch"
expect_failure "unlisted dependency patch" \
	env UML_DEPENDENCY_MANIFEST="$MANIFEST" \
		UML_DEPENDENCY_PATCH_ROOT="$METADATA" \
		"$PREPARE" --validate-manifest-only
rm "$METADATA/patches/unlisted.patch"

prepare "$PREPARED" >/dev/null
verify "$PREPARED" >/dev/null
[ "$(cat "$PREPARED/clean/payload.txt")" = baseline ] ||
	fail "unpatched dependency did not use the pinned commit"
[ "$(cat "$PREPARED/patched/payload.txt")" = patched ] ||
	fail "dependency patch series was not replayed"
[ "$(cat "$PREPARED/patched/added.txt")" = added ] ||
	fail "ordered dependency patch series was incomplete"
[ -x "$PREPARED/clean/sub/tool.sh" ] ||
	fail "prepared Git checkout lost executable status"
[ -z "$(git -C "$PREPARED/patched" remote)" ] ||
	fail "prepared dependency retained a network remote"
[ "$(git -C "$PREPARED/patched" rev-list --count "$BASE..HEAD")" -eq 2 ] ||
	fail "prepared dependency history has the wrong topology"

SPARSE_PREPARED="$TEST_ROOT/sparse-prepared"
SPARSE_CACHE_HEAD=$(git -C "$CACHE/git/sparse" rev-parse HEAD)
SPARSE_CACHE_STATUS=$(git -C "$CACHE/git/sparse" status --porcelain)
UML_DEPENDENCY_MANIFEST="$MANIFEST" \
UML_DEPENDENCY_PATCH_ROOT="$METADATA" \
	"$PREPARE" --product sparse-fixture --root "$SPARSE_PREPARED" \
		--cache "$CACHE" >/dev/null
[ -x "$SPARSE_PREPARED/sparse/sub/tool.sh" ] ||
	fail "sparse preparation omitted its selected source path"
[ ! -e "$SPARSE_PREPARED/sparse/other/unselected.txt" ] ||
	fail "sparse preparation materialized an unselected source path"
[ "$(git -C "$CACHE/git/sparse" rev-parse HEAD)" = "$SPARSE_CACHE_HEAD" ] ||
	fail "sparse materialization changed the shared cache HEAD"
[ "$(git -C "$CACHE/git/sparse" status --porcelain)" = "$SPARSE_CACHE_STATUS" ] ||
	fail "sparse materialization changed the shared cache worktree"

CONCURRENT="$TEST_ROOT/concurrent"
prepare "$CONCURRENT" >"$TEST_ROOT/concurrent-1.out" 2>&1 &
FIRST_PREPARER=$!
prepare "$CONCURRENT" >"$TEST_ROOT/concurrent-2.out" 2>&1 &
SECOND_PREPARER=$!
if ! wait "$FIRST_PREPARER"; then
	cat "$TEST_ROOT/concurrent-1.out" >&2
	fail "first concurrent preparer failed"
fi
if ! wait "$SECOND_PREPARER"; then
	cat "$TEST_ROOT/concurrent-2.out" >&2
	fail "second concurrent preparer failed"
fi
verify "$CONCURRENT" >/dev/null

printf '%s\n' tampered >>"$PREPARED/clean/payload.txt"
expect_failure "prepared-repository tampering" verify "$PREPARED"
git -C "$PREPARED/clean" restore payload.txt
verify "$PREPARED" >/dev/null

PATCH=$(find "$SERIES_DIR" -name '*.patch' -print -quit)
printf '%s\n' '# manually edited' >>"$PATCH"
expect_failure "manual patch-file editing" prepare "$TEST_ROOT/patch-tamper"
rm -rf "$SERIES_DIR"
cp -R "$TEST_ROOT/series-original" "$SERIES_DIR"

tail -n 1 "$MANIFEST.original" >>"$MANIFEST"
expect_failure "duplicate dependency identity" verify "$PREPARED"
cp "$MANIFEST.original" "$MANIFEST"

mkdir "$TEST_ROOT/existing-mismatch"
mkdir "$TEST_ROOT/existing-mismatch/clean"
printf '%s\n' retained >"$TEST_ROOT/existing-mismatch/clean/payload.txt"
expect_failure "replacement of an existing non-Git source" \
	prepare "$TEST_ROOT/existing-mismatch"
[ "$(cat "$TEST_ROOT/existing-mismatch/clean/payload.txt")" = retained ] ||
	fail "failed preparation replaced the mismatched source"

git -C "$CACHE/git/clean" remote set-url origin \
	https://example.invalid/wrong.git
expect_failure "cache origin tampering" prepare "$TEST_ROOT/cache-tamper"
git -C "$CACHE/git/clean" remote set-url origin \
	https://example.invalid/clean.git

mv "$CACHE/git/clean" "$CACHE/git/clean.absent"
expect_failure "missing offline Git cache" prepare "$TEST_ROOT/missing-cache"
mv "$CACHE/git/clean.absent" "$CACHE/git/clean"

printf '%s\n' "dependency Git preparation tests passed"
