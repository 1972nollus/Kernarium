#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Prepare or verify the managed Linux replay branch. The upstream input is an
# authenticated kernel.org release archive; Git exists locally only to replay
# and audit the checked-in patch series.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
METADATA_PATH="$ROOT_DIR/kernel/patches/metadata.env"
PATCH_DIR="$ROOT_DIR/kernel/patches/series"
MANIFEST_PATH="$PATCH_DIR/manifest.tsv"
MATERIALIZER="$ROOT_DIR/kernel/materialize-release.sh"

fail()
{
	printf '%s\n' "kernel/prepare-source.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/prepare-source.sh [options]

Prepare the pinned Darwin UML Linux source, or verify an existing prepared
checkout.  The source location must reside on a case-sensitive filesystem.

Options:
  --source PATH   Linux checkout to create or verify (default: ./linux or
                  UML_KERNEL_SOURCE_DIR)
  --cache PATH    Authenticated release cache (default: .build/kernel-cache or
                  UML_KERNEL_CACHE_DIR)
  --verify        Verify only; never download, switch branches, or apply patches
  --recreate      Explicitly discard a clean, stale managed replay branch and
                  replay the checked-in series from the pinned base
  --no-fetch      Prepare only from authenticated release files in the cache
  -h, --help      Show this help
EOF
}

[ -r "$METADATA_PATH" ] || fail "missing metadata: $METADATA_PATH"
[ -r "$MANIFEST_PATH" ] || fail "missing series manifest: $MANIFEST_PATH"
# The metadata file is tracked project data, not user-provided shell input.
# shellcheck disable=SC1090
. "$METADATA_PATH"

[ "${UML_KERNEL_SERIES_SCHEMA:-}" = 2 ] ||
	fail "unsupported kernel series schema"

SOURCE_DIR=${UML_KERNEL_SOURCE_DIR:-"$ROOT_DIR/linux"}
CACHE_ROOT=${UML_KERNEL_CACHE_DIR:-"$ROOT_DIR/.build/kernel-cache"}
VERIFY_ONLY=0
RECREATE=0
FETCH=1

while [ "$#" -gt 0 ]; do
	case "$1" in
	--source)
		[ "$#" -ge 2 ] || fail "--source requires a path"
		SOURCE_DIR=$2
		shift 2
		;;
	--cache)
		[ "$#" -ge 2 ] || fail "--cache requires a path"
		CACHE_ROOT=$2
		shift 2
		;;
	--verify)
		VERIFY_ONLY=1
		shift
		;;
	--recreate)
		RECREATE=1
		shift
		;;
	--no-fetch)
		FETCH=0
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		fail "unknown option: $1"
		;;
	esac
done

[ "$VERIFY_ONLY" -eq 0 ] || [ "$RECREATE" -eq 0 ] ||
	fail "--verify and --recreate cannot be used together"

command -v git >/dev/null 2>&1 || fail "git is required"
command -v shasum >/dev/null 2>&1 ||
	fail "stock macOS shasum is required to verify patch hashes"
[ -x "$MATERIALIZER" ] || fail "missing release materializer: $MATERIALIZER"
case "${UML_KERNEL_TRUSTED_KEY:-}" in
/*) TRUSTED_KEY=$UML_KERNEL_TRUSTED_KEY ;;
'') fail "trusted Linux release signing key path is missing" ;;
*) TRUSTED_KEY="$ROOT_DIR/$UML_KERNEL_TRUSTED_KEY" ;;
esac
[ -f "$TRUSTED_KEY" ] || fail "trusted Linux release signing key is missing: $TRUSTED_KEY"
actual_key_sha256=$(shasum -a 256 "$TRUSTED_KEY" | awk '{print $1}')
[ "$actual_key_sha256" = "${UML_KERNEL_TRUSTED_KEY_SHA256:-}" ] ||
	fail "trusted Linux release signing key hash changed: $TRUSTED_KEY"
"$ROOT_DIR/kernel/abi/sync.sh" --verify ||
	fail "host ABI copies are stale; run kernel/abi/sync.sh"

PARENT_DIR=$(dirname -- "$SOURCE_DIR")
SOURCE_NAME=$(basename -- "$SOURCE_DIR")
if [ "$VERIFY_ONLY" -eq 0 ]; then
	mkdir -p -- "$PARENT_DIR"
fi
[ -d "$PARENT_DIR" ] || fail "source parent does not exist: $PARENT_DIR"
PARENT_DIR=$(CDPATH= cd -- "$PARENT_DIR" && pwd -P)
SOURCE_DIR="$PARENT_DIR/$SOURCE_NAME"

assert_case_sensitive_directory()
{
	probe=$(mktemp -d "$1/.uml-case-sensitive.XXXXXX") ||
		fail "cannot create a case-sensitivity probe below $1"
	cleanup_probe()
	{
		rm -f -- "$probe/probe"
		rmdir -- "$probe" 2>/dev/null || true
	}
	trap cleanup_probe 0 HUP INT TERM
	: > "$probe/probe"
	if [ -e "$probe/PROBE" ]; then
		fail "$1 is case-insensitive; use a case-sensitive APFS volume or disk image"
	fi
	cleanup_probe
	trap - 0 HUP INT TERM
}

assert_case_sensitive_directory "$PARENT_DIR"

manifest_patch_ids=''
manifest_count=0
validate_manifest()
{
	tab=$(printf '\t')
	expected_order=1
	while IFS="$tab" read -r order filename patch_id digest subject; do
		case "$order" in
		\#*|order|'') continue ;;
		esac
		[ "$order" = "$expected_order" ] ||
			fail "series manifest order $order is not $expected_order"
		case "$filename" in
		''|*/*|*..*|*[!A-Za-z0-9._-]*)
			fail "unsafe patch filename in manifest: $filename"
			;;
		esac
		patch="$PATCH_DIR/$filename"
		[ -f "$patch" ] || fail "manifest patch is missing: $patch"
		actual_digest=$(shasum -a 256 "$patch" | awk '{print $1}')
		[ "$actual_digest" = "$digest" ] ||
			fail "patch hash mismatch for $filename"
		actual_patch_id=$(git patch-id --stable < "$patch" | awk '{print $1}')
		[ -n "$actual_patch_id" ] && [ "$actual_patch_id" = "$patch_id" ] ||
			fail "stable patch-id mismatch for $filename"
		manifest_patch_ids="${manifest_patch_ids}${patch_id}
"
		manifest_count=$((manifest_count + 1))
		expected_order=$((expected_order + 1))
	done < "$MANIFEST_PATH"
	[ "$manifest_count" -eq "$UML_KERNEL_PATCH_COUNT" ] ||
		fail "manifest has $manifest_count patches; metadata requires $UML_KERNEL_PATCH_COUNT"
}

validate_manifest

if [ ! -d "$SOURCE_DIR/.git" ]; then
	[ "$VERIFY_ONLY" -eq 0 ] || fail "Linux checkout is missing: $SOURCE_DIR"
	if [ "$FETCH" -eq 1 ]; then
		"$MATERIALIZER" --source "$SOURCE_DIR" --cache "$CACHE_ROOT"
	else
		"$MATERIALIZER" --source "$SOURCE_DIR" --cache "$CACHE_ROOT" --no-fetch
	fi
fi

require_source_config()
{
	key=$1
	expected=$2
	actual=$(git -C "$SOURCE_DIR" config --local --get "$key" 2>/dev/null || true)
	[ "$actual" = "$expected" ] ||
		fail "source provenance $key is '$actual', expected '$expected'"
}

verify_release_identity()
{
	git -C "$SOURCE_DIR" show-ref --verify --quiet "$UML_KERNEL_BASE_REF" ||
		fail "authenticated release base ref is missing: $UML_KERNEL_BASE_REF"
	BASE_COMMIT=$(git -C "$SOURCE_DIR" rev-parse "$UML_KERNEL_BASE_REF^{commit}")
	base_tree=$(git -C "$SOURCE_DIR" rev-parse "$UML_KERNEL_BASE_REF^{tree}")
	[ "$base_tree" = "$UML_KERNEL_PACKAGED_TREE" ] ||
		fail "packaged release base has tree $base_tree; expected $UML_KERNEL_PACKAGED_TREE"
	base_with_parents=$(git -C "$SOURCE_DIR" rev-list --parents -n 1 "$BASE_COMMIT")
	[ "$base_with_parents" = "$BASE_COMMIT" ] ||
		fail "packaged release base must be the root of the local replay repository"
	require_source_config uml.source.releaseVersion "$UML_KERNEL_RELEASE_VERSION"
	require_source_config uml.source.archiveUrl "$UML_KERNEL_ARCHIVE_URL"
	require_source_config uml.source.archiveSha256 "$UML_KERNEL_ARCHIVE_SHA256"
	require_source_config uml.source.signatureUrl "$UML_KERNEL_SIGNATURE_URL"
	require_source_config uml.source.signatureSha256 "$UML_KERNEL_SIGNATURE_SHA256"
	require_source_config uml.source.signerFingerprint "$UML_KERNEL_SIGNER_FINGERPRINT"
	require_source_config uml.source.upstreamTag "$UML_KERNEL_UPSTREAM_TAG"
	require_source_config uml.source.upstreamTagCommit "$UML_KERNEL_UPSTREAM_TAG_COMMIT"
	require_source_config uml.source.upstreamTagTree "$UML_KERNEL_UPSTREAM_TAG_TREE"
	require_source_config uml.source.packagedTree "$UML_KERNEL_PACKAGED_TREE"
	require_source_config uml.source.baseRef "$UML_KERNEL_BASE_REF"
}

BASE_COMMIT=''
verify_release_identity

require_clean_checkout()
{
	if [ -n "$(git -C "$SOURCE_DIR" status --porcelain --untracked-files=all)" ]; then
		fail "managed Linux checkout is dirty: $SOURCE_DIR"
	fi
}

verify_managed_branch()
{
	actual_head=$(git -C "$SOURCE_DIR" rev-parse HEAD)
	branch_tree=$(git -C "$SOURCE_DIR" rev-parse "$UML_KERNEL_PATCH_BRANCH^{tree}")
	[ "$branch_tree" = "$UML_KERNEL_FINAL_TREE" ] ||
		fail "managed branch has tree $branch_tree; expected $UML_KERNEL_FINAL_TREE (use --recreate only after reviewing it)"
	actual_patch_ids=$(git -C "$SOURCE_DIR" rev-list --reverse \
		"$BASE_COMMIT..$UML_KERNEL_PATCH_BRANCH" |
		while IFS= read -r commit; do
			git -C "$SOURCE_DIR" show --pretty=format: "$commit" |
				git patch-id --stable | awk '{print $1}'
		done)
	[ "$actual_patch_ids" = "$(printf '%s' "$manifest_patch_ids")" ] ||
		fail "managed branch patch history does not match $MANIFEST_PATH"
	git -C "$SOURCE_DIR" merge-base --is-ancestor "$BASE_COMMIT" \
		"$UML_KERNEL_PATCH_BRANCH" ||
		fail "managed branch is not based on the authenticated packaged release"
	"$ROOT_DIR/kernel/abi/sync.sh" --kernel-source "$SOURCE_DIR" --verify ||
		fail "managed Linux ABI copies are stale"
	printf '%s\n' "verified managed branch HEAD $actual_head against canonical tree $UML_KERNEL_FINAL_TREE" >&2
}

branch_exists=0
if git -C "$SOURCE_DIR" show-ref --verify --quiet \
	"refs/heads/$UML_KERNEL_PATCH_BRANCH"; then
	branch_exists=1
fi

if [ "$VERIFY_ONLY" -eq 1 ]; then
	[ "$branch_exists" -eq 1 ] ||
		fail "managed replay branch is missing: $UML_KERNEL_PATCH_BRANCH"
	current_branch=$(git -C "$SOURCE_DIR" branch --show-current)
	[ "$current_branch" = "$UML_KERNEL_PATCH_BRANCH" ] ||
		fail "HEAD is '$current_branch'; expected $UML_KERNEL_PATCH_BRANCH"
	require_clean_checkout
	verify_managed_branch
	printf '%s\n' "verified $SOURCE_DIR on $UML_KERNEL_PATCH_BRANCH"
	exit 0
fi

require_clean_checkout

if [ "$branch_exists" -eq 1 ] && [ "$RECREATE" -eq 0 ]; then
	git -C "$SOURCE_DIR" switch "$UML_KERNEL_PATCH_BRANCH"
	verify_managed_branch
	printf '%s\n' "prepared $SOURCE_DIR on $UML_KERNEL_PATCH_BRANCH"
	exit 0
fi

if [ "$branch_exists" -eq 1 ]; then
	# Recreate is intentionally conservative.  It only removes a branch whose
	# history is based on the pinned base and has no more commits than the
	# checked-in series; anything else requires a human review.
	managed_commit_count=$(git -C "$SOURCE_DIR" rev-list --count \
		"$BASE_COMMIT..$UML_KERNEL_PATCH_BRANCH")
	git -C "$SOURCE_DIR" merge-base --is-ancestor "$BASE_COMMIT" \
		"$UML_KERNEL_PATCH_BRANCH" ||
		fail "refusing to recreate branch not based on the pinned Linux base"
	[ "$managed_commit_count" -le "$UML_KERNEL_PATCH_COUNT" ] ||
		fail "refusing to recreate branch with $managed_commit_count commits; review it manually"
	git -C "$SOURCE_DIR" switch --detach "$BASE_COMMIT"
	git -C "$SOURCE_DIR" branch -D "$UML_KERNEL_PATCH_BRANCH"
fi

git -C "$SOURCE_DIR" switch -c "$UML_KERNEL_PATCH_BRANCH" "$BASE_COMMIT"
# Patch authors and author dates come from the checked-in mail patches. The
# replay committer identity is explicit and its timestamp is the real replay
# time; neither generated commit ID is treated as a source identity.
unset GIT_AUTHOR_DATE GIT_COMMITTER_DATE
tab=$(printf '\t')
while IFS="$tab" read -r order filename patch_id digest subject; do
	case "$order" in
	\#*|order|'') continue ;;
	esac
	if ! GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$SOURCE_DIR" \
			-c user.name='linux-um-darwin patch replay' \
			-c user.email='noreply@linux-um-darwin.invalid' \
			-c core.hooksPath=/dev/null \
			am --3way --no-gpg-sign "$PATCH_DIR/$filename"; then
		git -C "$SOURCE_DIR" am --abort >/dev/null 2>&1 || true
		fail "could not apply patch $filename to the authenticated packaged release"
	fi
done < "$MANIFEST_PATH"

verify_managed_branch
require_clean_checkout
printf '%s\n' "prepared $SOURCE_DIR on $UML_KERNEL_PATCH_BRANCH"
