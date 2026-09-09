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

# Hash-pinned archive acquisition publishes only authenticated bytes and
# never installs packages. The curl fixture keeps this test fully offline.
mkdir -p "$TEST_ROOT/archive-tools"
cat >"$TEST_ROOT/archive-tools/curl" <<'EOF_ARCHIVE_CURL'
#!/bin/sh
set -eu
while [ "$#" -gt 0 ]; do
	case "$1" in
	--output) destination=$2; shift 2 ;;
	*) shift ;;
	esac
done
cp "$UML_FIXTURE_ARCHIVE" "$destination"
EOF_ARCHIVE_CURL
chmod +x "$TEST_ROOT/archive-tools/curl"
printf 'authenticated archive fixture\n' >"$TEST_ROOT/archive-payload"
ARCHIVE_SHA=$(sha256_file "$TEST_ROOT/archive-payload")
ARCHIVE_MANIFEST="$TEST_ROOT/archives.tsv"
printf 'name\turl\tsha256\nfixture.deb\thttps://fixture.invalid/fixture.deb\t%s\n' \
	"$ARCHIVE_SHA" >"$ARCHIVE_MANIFEST"
env PATH="$TEST_ROOT/archive-tools:$PATH" \
	UML_FIXTURE_ARCHIVE="$TEST_ROOT/archive-payload" \
	"$PREPARE" --archive-manifest "$ARCHIVE_MANIFEST" \
		--cache "$TEST_ROOT/archive-cache" >/dev/null
"$PREPARE" --archive-manifest "$ARCHIVE_MANIFEST" \
	--cache "$TEST_ROOT/archive-cache" --no-fetch >/dev/null
cmp "$TEST_ROOT/archive-payload" \
	"$TEST_ROOT/archive-cache/archives/$ARCHIVE_SHA-fixture.deb" >/dev/null ||
	fail "archive publication changed authenticated bytes"
printf corrupt >"$TEST_ROOT/archive-cache/archives/$ARCHIVE_SHA-fixture.deb"
FAILURE_NUMBER=0
expect_failure 'corrupt cached archive' "$PREPARE" \
	--archive-manifest "$ARCHIVE_MANIFEST" --cache "$TEST_ROOT/archive-cache" --verify
expect_failure 'absent offline archive' "$PREPARE" \
	--archive-manifest "$ARCHIVE_MANIFEST" --cache "$TEST_ROOT/absent-archive-cache" --no-fetch
expect_failure 'absent verification cache' "$PREPARE" \
	--archive-manifest "$ARCHIVE_MANIFEST" --cache "$TEST_ROOT/absent-verify-cache" --verify
[ ! -e "$TEST_ROOT/absent-verify-cache" ] || fail "archive verification created a cache"
sed 's/fixture.deb/name/g' "$ARCHIVE_MANIFEST" >"$TEST_ROOT/reserved-archives.tsv"
expect_failure 'reserved archive name' "$PREPARE" \
	--archive-manifest "$TEST_ROOT/reserved-archives.tsv" --validate-manifest-only
sed 's/fixture.deb/..\/fixture.deb/g' "$ARCHIVE_MANIFEST" >"$TEST_ROOT/unsafe-archives.tsv"
expect_failure 'unsafe archive name' "$PREPARE" \
	--archive-manifest "$TEST_ROOT/unsafe-archives.tsv" --validate-manifest-only
sed "s/$ARCHIVE_SHA/0000000000000000000000000000000000000000000000000000000000000000/" \
	"$ARCHIVE_MANIFEST" >"$TEST_ROOT/wrong-hash-archives.tsv"
expect_failure 'archive download hash mismatch' \
	env PATH="$TEST_ROOT/archive-tools:$PATH" \
		UML_FIXTURE_ARCHIVE="$TEST_ROOT/archive-payload" "$PREPARE" \
		--archive-manifest "$TEST_ROOT/wrong-hash-archives.tsv" \
		--cache "$TEST_ROOT/rejected-archive-cache"
[ -z "$(find "$TEST_ROOT/rejected-archive-cache/archives" -type f -print)" ] ||
	fail "rejected archive left a published or temporary payload"

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

# Ref discovery stays in the network-owning workflow, but immutable source
# preparation still accepts only a pinned commit. Redirect this test's HTTPS
# endpoint to the local fixture without changing user Git configuration.
git -C "$UPSTREAM" tag fixture-lightweight "$BASE"
git -C "$UPSTREAM" tag -a fixture-annotated "$BASE" -m 'fixture tag'
resolve_ref()
{
	env GIT_CONFIG_COUNT=1 \
		GIT_CONFIG_KEY_0="url.file://$UPSTREAM.insteadOf" \
		GIT_CONFIG_VALUE_0=https://fixture.invalid/upstream.git \
		"$PREPARE" --resolve-ref https://fixture.invalid/upstream.git "$@"
}
for ref in fixture-lightweight fixture-annotated; do
	[ "$(resolve_ref "refs/tags/$ref")" = "$BASE" ] ||
		fail "dependency ref did not resolve to its peeled commit"
done
expect_failure 'missing dependency ref' resolve_ref refs/tags/absent
expect_failure 'wildcard dependency ref' resolve_ref 'refs/tags/fixture-*'
expect_failure 'unqualified dependency ref' resolve_ref fixture-lightweight
expect_failure 'offline ref resolution' resolve_ref refs/tags/fixture-lightweight --no-fetch
expect_failure 'non-HTTPS ref resolution' \
	"$PREPARE" --resolve-ref "$UPSTREAM" refs/tags/fixture-lightweight

TAG_DESTINATION="$TEST_ROOT/tag-destination"
git clone --quiet --no-tags "$UPSTREAM" "$TAG_DESTINATION"
TAG_OBJECT=$(git -C "$UPSTREAM" rev-parse refs/tags/fixture-annotated)
materialize_tag()
{
	env GIT_CONFIG_COUNT=1 \
		GIT_CONFIG_KEY_0="url.file://$UPSTREAM.insteadOf" \
		GIT_CONFIG_VALUE_0=https://fixture.invalid/upstream.git \
		"$PREPARE" --materialize-tag https://fixture.invalid/upstream.git "$@"
}
expect_failure 'wrong pinned tag object' materialize_tag \
	refs/tags/fixture-annotated "$BASE" "$BASE" "$TAG_DESTINATION"
expect_failure 'wrong peeled tag commit' materialize_tag \
	refs/tags/fixture-annotated "$TAG_OBJECT" "$TAG_OBJECT" "$TAG_DESTINATION"
if git -C "$TAG_DESTINATION" show-ref --verify refs/tags/fixture-annotated >/dev/null 2>&1; then
	fail "failed tag verification published a tag ref"
fi
materialize_tag refs/tags/fixture-annotated "$TAG_OBJECT" "$BASE" "$TAG_DESTINATION"
[ "$(git -C "$TAG_DESTINATION" rev-parse refs/tags/fixture-annotated)" = "$TAG_OBJECT" ] &&
	[ "$(git -C "$TAG_DESTINATION" rev-parse HEAD)" = "$BASE" ] &&
	[ -z "$(git -C "$TAG_DESTINATION" status --porcelain)" ] ||
	fail "tag materialization changed the prepared checkout"
expect_failure 'lightweight tag materialization' materialize_tag \
	refs/tags/fixture-lightweight "$BASE" "$BASE" "$TAG_DESTINATION"
expect_failure 'branch materialization' materialize_tag \
	refs/heads/master "$BASE" "$BASE" "$TAG_DESTINATION"
expect_failure 'offline tag materialization' materialize_tag \
	refs/tags/fixture-annotated "$TAG_OBJECT" "$BASE" "$TAG_DESTINATION" --no-fetch
git -C "$TAG_DESTINATION" update-ref refs/tags/fixture-annotated "$BASE"
expect_failure 'existing mismatched tag' materialize_tag \
	refs/tags/fixture-annotated "$TAG_OBJECT" "$BASE" "$TAG_DESTINATION"

printf '%s\n' patched >"$UPSTREAM/payload.txt"
git -C "$UPSTREAM" commit --quiet -am 'fixture: first maintained change'
printf '%s\n' added >"$UPSTREAM/added.txt"
printf '\000\001\002\377' >"$UPSTREAM/added.bin"
git -C "$UPSTREAM" add added.txt added.bin
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

# Model the large sparse-source cache used by the Apple GPU product: only the
# selected cone is present, and the promisor URL is deliberately unreachable.
# Publishing from this cache must remain local and must not let upload-pack
# recursively lazy-fetch through the staged repository's local origin.
git -C "$UPSTREAM" config uploadpack.allowFilter true
git clone --quiet --filter=blob:none --no-checkout \
	"file://$UPSTREAM" "$CACHE/git/partial"
git -C "$CACHE/git/partial" sparse-checkout init --cone
git -C "$CACHE/git/partial" sparse-checkout set --cone sub
git -C "$CACHE/git/partial" switch --detach "$BASE" >/dev/null
git -C "$CACHE/git/partial" remote set-url origin \
	https://example.invalid/partial.git
git -C "$CACHE/git/partial" update-ref refs/heads/uml-pin "$BASE"
git -C "$CACHE/git/partial" config uploadpack.allowFilter true
printf '%s\n' "$BASE" >"$CACHE/git/partial/.git/shallow"
[ -n "$(env GIT_NO_LAZY_FETCH=1 git -C "$CACHE/git/partial" \
	rev-list --objects --missing=print "$BASE" | sed -n 's/^?//p' | sed -n '1p')" ] ||
	fail "partial sparse fixture unexpectedly contains the whole commit"

cat >"$MANIFEST" <<EOF
name	version	products	repository	commit	source_paths	patch_series
clean	1.0	fixture	https://example.invalid/clean.git	$BASE	.	-
patched	1.0	fixture	https://example.invalid/patched.git	$BASE	.	patches/patched/series.tsv
sparse	1.0	sparse-fixture	https://example.invalid/sparse.git	$BASE	sub	-
partial	1.0	partial-fixture	https://example.invalid/partial.git	$BASE	sub	-
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
cmp "$UPSTREAM/added.bin" "$PREPARED/patched/added.bin" >/dev/null ||
	fail "binary dependency patch was not replayed exactly"
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

PARTIAL_PREPARED="$TEST_ROOT/partial-prepared"
UML_DEPENDENCY_MANIFEST="$MANIFEST" \
UML_DEPENDENCY_PATCH_ROOT="$METADATA" \
	"$PREPARE" --product partial-fixture --root "$PARTIAL_PREPARED" \
		--cache "$CACHE" --no-fetch >/dev/null
[ -x "$PARTIAL_PREPARED/partial/sub/tool.sh" ] ||
	fail "partial-cache publication omitted its selected source path"
[ ! -e "$PARTIAL_PREPARED/partial/other/unselected.txt" ] ||
	fail "partial-cache publication materialized an unselected source path"
[ -z "$(git -C "$PARTIAL_PREPARED/partial" remote)" ] ||
	fail "partial-cache publication retained its local promisor remote"

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

# Foreign-owned protocol metadata is authenticated in place, never copied
# into the dependency patch inventory or resolved through arbitrary '..'.
ROOTFS_METADATA="$TEST_ROOT/tools/rootfs/vulkan"
ROOTFS_SERIES="$ROOTFS_METADATA/patches/venus-protocol"
mkdir -p "$ROOTFS_METADATA/patches"
cp -R "$TEST_ROOT/series-original" "$ROOTFS_SERIES"
for name in venus-protocol vulkan-headers; do
	git clone --quiet "$UPSTREAM" "$CACHE/git/$name"
	git -C "$CACHE/git/$name" remote set-url origin \
		"https://example.invalid/$name.git"
	git -C "$CACHE/git/$name" update-ref refs/heads/uml-pin "$BASE"
done
cat >>"$MANIFEST" <<EOF
vulkan-headers	1.0	apple-gpu-vulkan-experiment	https://example.invalid/vulkan-headers.git	$BASE	.	-
venus-protocol	1.0	apple-gpu-vulkan-experiment	https://example.invalid/venus-protocol.git	$BASE	.	rootfs-vulkan:patches/venus-protocol/series.tsv
EOF
cat >"$ROOTFS_METADATA/sources.tsv" <<EOF
component	repository	base_commit	base_tree	release_ref	release_object	series
vulkan-headers	https://example.invalid/vulkan-headers.git	$BASE	-	-	-	-
venus-protocol	https://example.invalid/venus-protocol.git	$BASE	-	-	-	patches/venus-protocol/series.tsv
EOF
cp "$MANIFEST" "$TEST_ROOT/paired-manifest.tsv"
cp "$ROOTFS_METADATA/sources.tsv" "$TEST_ROOT/paired-sources.tsv"
paired_prepare()
{
	UML_DEPENDENCY_MANIFEST="$MANIFEST" UML_DEPENDENCY_PATCH_ROOT="$METADATA" \
		"$PREPARE" --product apple-gpu-vulkan-experiment \
		--root "$TEST_ROOT/paired-prepared" --cache "$CACHE" --no-fetch
}
paired_prepare >/dev/null
[ "$(cat "$TEST_ROOT/paired-prepared/venus-protocol/payload.txt")" = patched ] ||
	fail "foreign-owned protocol topics were not replayed"
[ ! -e "$METADATA/patches/venus-protocol" ] ||
	fail "preparation duplicated foreign-owned protocol metadata"
for invalid in \
	'unknown:patches/venus-protocol/series.tsv' \
	'rootfs-vulkan:../patches/venus-protocol/series.tsv' \
	'rootfs-vulkan:/patches/venus-protocol/series.tsv' \
	'rootfs-vulkan:patches/%2e%2e/venus-protocol/series.tsv'; do
	awk -F '\t' -v replacement="$invalid" 'BEGIN { OFS = FS }
		$1 == "venus-protocol" { $7 = replacement } { print }' \
		"$TEST_ROOT/paired-manifest.tsv" >"$MANIFEST"
	expect_failure "unsafe foreign series $invalid" paired_prepare
done
cp "$TEST_ROOT/paired-manifest.tsv" "$MANIFEST"
mv "$ROOTFS_SERIES" "$TEST_ROOT/foreign-series"
ln -s "$TEST_ROOT/foreign-series" "$ROOTFS_SERIES"
expect_failure "foreign series symlink escape" paired_prepare
rm "$ROOTFS_SERIES"
mv "$TEST_ROOT/foreign-series" "$ROOTFS_SERIES"
for component in venus-protocol vulkan-headers; do
	awk -F '\t' -v component="$component" 'BEGIN { OFS = FS }
		$1 == component { $3 = "0000000000000000000000000000000000000000" }
		{ print }' "$TEST_ROOT/paired-sources.tsv" >"$ROOTFS_METADATA/sources.tsv"
	expect_failure "mismatched paired $component source" paired_prepare
done
cp "$TEST_ROOT/paired-sources.tsv" "$ROOTFS_METADATA/sources.tsv"
awk -F '\t' '$1 != "vulkan-headers"' "$TEST_ROOT/paired-manifest.tsv" >"$MANIFEST"
expect_failure "missing paired SDK declaration" paired_prepare
cp "$TEST_ROOT/paired-manifest.tsv" "$MANIFEST"
paired_prepare >/dev/null

TOOLCHAIN_MANIFEST="$TEST_ROOT/toolchains.tsv"
TOOLCHAIN_CACHE="$TEST_ROOT/toolchain-cache"
TOOLCHAIN_ROOT="$TEST_ROOT/toolchains"
TOOLCHAIN_ARCHIVE_ROOT=fixture-toolchain-root
TOOLCHAIN_SOURCE="$TEST_ROOT/$TOOLCHAIN_ARCHIVE_ROOT"
mkdir -p "$TOOLCHAIN_CACHE/archives" "$TOOLCHAIN_SOURCE/bin"
for binary in clang llvm-dlltool lld-link; do
	printf '#!/bin/sh\nexit 0\n' >"$TOOLCHAIN_SOURCE/bin/$binary"
	chmod +x "$TOOLCHAIN_SOURCE/bin/$binary"
done
TOOLCHAIN_ARCHIVE="$TEST_ROOT/fixture-toolchain.tar.xz"
tar -cJf "$TOOLCHAIN_ARCHIVE" -C "$TEST_ROOT" "$TOOLCHAIN_ARCHIVE_ROOT"
TOOLCHAIN_SHA=$(sha256_file "$TOOLCHAIN_ARCHIVE")
cat >"$TOOLCHAIN_MANIFEST" <<EOF
name	version	host	archive_url	sha256	archive_root
fixture-toolchain	1.0	test-host	https://example.invalid/fixture-toolchain.tar.xz	$TOOLCHAIN_SHA	$TOOLCHAIN_ARCHIVE_ROOT
EOF
cp "$TOOLCHAIN_ARCHIVE" \
	"$TOOLCHAIN_CACHE/archives/fixture-toolchain-1.0-$TOOLCHAIN_SHA.tar.xz"

prepare_toolchain_fixture()
{
	UML_DEPENDENCY_TOOLCHAIN_MANIFEST="$TOOLCHAIN_MANIFEST" \
		"$PREPARE" --toolchain fixture-toolchain \
		--toolchain-root "$TOOLCHAIN_ROOT" --cache "$TOOLCHAIN_CACHE" \
		--no-fetch
}

verify_toolchain_fixture()
{
	UML_DEPENDENCY_TOOLCHAIN_MANIFEST="$TOOLCHAIN_MANIFEST" \
		"$PREPARE" --toolchain fixture-toolchain \
		--toolchain-root "$TOOLCHAIN_ROOT" --cache "$TOOLCHAIN_CACHE" \
		--verify
}

prepare_toolchain_fixture >/dev/null
verify_toolchain_fixture >/dev/null
PREPARED_TOOLCHAIN="$TOOLCHAIN_ROOT/fixture-toolchain-1.0-test-host"
[ -x "$PREPARED_TOOLCHAIN/bin/clang" ] ||
	fail "binary toolchain preparation omitted clang"
cp "$PREPARED_TOOLCHAIN/.uml-toolchain-receipt" \
	"$TEST_ROOT/toolchain-receipt.original"
printf '%s\n' tampered >>"$PREPARED_TOOLCHAIN/.uml-toolchain-receipt"
expect_failure "binary toolchain receipt tampering" verify_toolchain_fixture
cp "$TEST_ROOT/toolchain-receipt.original" \
	"$PREPARED_TOOLCHAIN/.uml-toolchain-receipt"
verify_toolchain_fixture >/dev/null

tail -n 1 "$TOOLCHAIN_MANIFEST" >>"$TOOLCHAIN_MANIFEST"
expect_failure "duplicate binary toolchain identity" \
	env UML_DEPENDENCY_TOOLCHAIN_MANIFEST="$TOOLCHAIN_MANIFEST" \
		"$PREPARE" --toolchain fixture-toolchain --validate-manifest-only
sed -n '1,2p' "$TOOLCHAIN_MANIFEST" >"$TEST_ROOT/toolchains.restored"
mv "$TEST_ROOT/toolchains.restored" "$TOOLCHAIN_MANIFEST"

rm -rf "$TOOLCHAIN_ROOT"
mv "$TOOLCHAIN_CACHE/archives/fixture-toolchain-1.0-$TOOLCHAIN_SHA.tar.xz" \
	"$TEST_ROOT/toolchain-cache-archive"
expect_failure "missing offline binary toolchain archive" \
	prepare_toolchain_fixture

printf '%s\n' "dependency preparation tests passed"
