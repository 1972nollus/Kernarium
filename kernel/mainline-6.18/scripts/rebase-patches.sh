#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Rebase the maintained kernel commits onto the pinned stable commit, then
# replace the checked-in mail series exclusively with git format-patch output.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
POLICY="$ROOT_DIR/kernel/common/tooling/verify-series-policy.py"
SOURCE_LOCATION="$LINE_ROOT/scripts/source-location.sh"
SLICE_SELECTOR="$LINE_ROOT/scripts/select-slice.sh"
REQUESTED_SOURCE=
SLICE=arm64

fail()
{
	printf '%s\n' "kernel/mainline-6.18/scripts/rebase-patches.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/mainline-6.18/scripts/rebase-patches.sh \
       [--slice arm64|x86_64] [--source CONFIGURED_PATH]

PATH must be the clean maintained kernel topic branch. To modify a patch, use
`git rebase -i <selected-series-parent>` in that checkout, amend/reorder the
topic commits, and then run this command. Common and x86_64 overlay patches are
generated into separate series; never edit generated .patch files.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--slice)
		[ "$#" -ge 2 ] || fail "--slice requires arm64 or x86_64"
		SLICE=$2
		shift 2
		;;
	--source)
		[ "$#" -ge 2 ] || fail "--source requires a path"
		REQUESTED_SOURCE=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -r "$SLICE_SELECTOR" ] || fail "missing slice selector: $SLICE_SELECTOR"
# shellcheck disable=SC1090
. "$SLICE_SELECTOR"
uml_kernel_select_slice "$LINE_ROOT" "$SLICE" allow-stale-parent ||
	fail "cannot select kernel slice: $SLICE"
METADATA=$UML_KERNEL_METADATA_PATH
SERIES_DIR=$UML_KERNEL_PATCH_DIR
MANIFEST=$UML_KERNEL_MANIFEST_PATH
POLICY_ROOT=$UML_KERNEL_POLICY_ROOT

[ -x "$SOURCE_LOCATION" ] || fail "missing source-location verifier: $SOURCE_LOCATION"
SOURCE=$("$SOURCE_LOCATION" --require-source) ||
	fail "kernel source configuration is invalid"
if [ -n "$REQUESTED_SOURCE" ] && [ "$REQUESTED_SOURCE" != "$SOURCE" ]; then
	fail "--source must match configured UML_KERNEL_SOURCE_DIR: $SOURCE"
fi
[ -r "$METADATA" ] || fail "missing metadata: $METADATA"
[ -d "$SOURCE/.git" ] || fail "source is not a Git working tree: $SOURCE"
[ -z "$(git -C "$SOURCE" status --porcelain --untracked-files=all)" ] ||
	fail "source working tree is dirty"
branch=$(git -C "$SOURCE" branch --show-current)
[ "$branch" = "$UML_KERNEL_PATCH_BRANCH" ] ||
	fail "source branch is '$branch', expected '$UML_KERNEL_PATCH_BRANCH'"
BASE_COMMIT=$UML_KERNEL_UPSTREAM_COMMIT
REBASE_OLD_BASE=$BASE_COMMIT
if [ "$UML_KERNEL_SLICE" = x86_64 ]; then
	common_tree=$(git -C "$SOURCE" rev-parse \
		"$UML_KERNEL_COMMON_PATCH_BRANCH^{tree}" 2>/dev/null || true)
	[ "$common_tree" = "$UML_KERNEL_COMMON_FINAL_TREE" ] ||
		fail "x86_64 parent is not the canonical common kernel tree"
	# Tree equivalence is insufficient: the overlay must inherit the exact
	# ordered common topic history so its generated provenance remains complete.
	"$POLICY" --line-root "$LINE_ROOT" topology \
		--source "$SOURCE" --base "$UML_KERNEL_UPSTREAM_COMMIT" \
		--tip "$UML_KERNEL_COMMON_PATCH_BRANCH" >/dev/null
	"$POLICY" --line-root "$LINE_ROOT" policy \
		--source "$SOURCE" --base "$UML_KERNEL_UPSTREAM_COMMIT" \
		--tip "$UML_KERNEL_COMMON_PATCH_BRANCH" >/dev/null
	BASE_COMMIT=$(git -C "$SOURCE" rev-parse \
		"$UML_KERNEL_COMMON_PATCH_BRANCH^{commit}")
	REBASE_OLD_BASE=$(git -C "$SOURCE" rev-parse \
		"HEAD~$UML_KERNEL_PATCH_COUNT^{commit}" 2>/dev/null || true)
	[ -n "$REBASE_OLD_BASE" ] ||
		fail "x86_64 branch is shorter than its tracked overlay"
	old_parent_tree=$(git -C "$SOURCE" rev-parse \
		"$REBASE_OLD_BASE^{tree}")
	if [ "$old_parent_tree" != "$UML_KERNEL_PARENT_TREE" ] &&
	   [ "$old_parent_tree" != "$UML_KERNEL_COMMON_FINAL_TREE" ]; then
		fail "x86_64 overlay parent tree is neither its tracked parent nor the current common tree"
	fi
	[ -z "$(git -C "$SOURCE" rev-list --merges \
		"$REBASE_OLD_BASE..HEAD")" ] ||
		fail "x86_64 overlay contains a merge"
fi
[ "$(git -C "$SOURCE" rev-parse "$BASE_COMMIT^{commit}" 2>/dev/null || true)" = \
	"$BASE_COMMIT" ] || fail "selected series parent commit is absent"
if [ "$UML_KERNEL_SLICE" = arm64 ]; then
	git -C "$SOURCE" merge-base --is-ancestor "$BASE_COMMIT" HEAD ||
		fail "maintained branch is not based on the selected series parent"
	[ -z "$(git -C "$SOURCE" rev-list --merges \
		"$BASE_COMMIT..HEAD")" ] || fail "maintained branch has a merge"
fi

# Git history is the editing authority. This is deliberately run even when it
# is a no-op, so series regeneration always follows an explicit rebase step.
GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
	git -C "$SOURCE" -c core.hooksPath=/dev/null \
		-c user.name='Yi Xie' \
		-c user.email='imxieyi@users.noreply.github.com' \
		rebase --onto "$BASE_COMMIT" "$REBASE_OLD_BASE"

if [ "$UML_KERNEL_SLICE" = x86_64 ] &&
   [ "$UML_KERNEL_PARENT_TREE" != "$UML_KERNEL_COMMON_FINAL_TREE" ]; then
	fail "x86_64 branch was rebased; update its parent/final tree metadata and rerun"
fi

commit_count=$(git -C "$SOURCE" rev-list --count \
	"$BASE_COMMIT..HEAD")
[ "$commit_count" -eq "$UML_KERNEL_PATCH_COUNT" ] ||
	fail "maintained branch has $commit_count commits, expected $UML_KERNEL_PATCH_COUNT"
[ -z "$(git -C "$SOURCE" rev-list --merges \
	"$BASE_COMMIT..HEAD")" ] || fail "rebased branch has a merge"
final_tree=$(git -C "$SOURCE" rev-parse HEAD^{tree})
[ "$final_tree" = "$UML_KERNEL_FINAL_TREE" ] ||
	fail "rebased final tree is $final_tree, expected $UML_KERNEL_FINAL_TREE"

temporary=$(mktemp -d "$SERIES_DIR/.format-patch.XXXXXX")
cleanup()
{
	[ ! -d "${temporary:-}" ] || rm -rf -- "$temporary"
}
trap cleanup EXIT HUP INT TERM
NUMBERING=--numbered
if [ "$UML_KERNEL_PATCH_COUNT" -eq 1 ]; then
	NUMBERING=--no-numbered
fi
git -C "$SOURCE" format-patch "$NUMBERING" --no-signature \
	--output-directory "$temporary" \
	"$BASE_COMMIT..HEAD" >/dev/null
generated_count=$(find "$temporary" -type f -name '*.patch' | wc -l | tr -d ' ')
[ "$generated_count" -eq "$UML_KERNEL_PATCH_COUNT" ] ||
	fail "git format-patch emitted $generated_count patches"

manifest_tmp="$temporary/manifest.tsv"
{
	printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later'
	printf '%s\n' '# Ordered final-state topic identity manifest. Fields are order, file, stable'
	printf '%s\n' '# patch-id, SHA-256 of the git-generated patch, and commit subject.'
	printf 'order\tfilename\tstable_patch_id\tsha256\tsubject\n'
	order=1
	find "$temporary" -type f -name '*.patch' -print | LC_ALL=C sort |
	while IFS= read -r patch; do
		filename=$(basename -- "$patch")
		patch_id=$(git patch-id --stable < "$patch" | awk 'NF { print $1; exit }')
		digest=$(shasum -a 256 "$patch" | awk '{print $1}')
		subject=$(git mailinfo /dev/null /dev/null < "$patch" |
			sed -n 's/^Subject: //p')
		[ -n "$patch_id" ] && [ -n "$subject" ] ||
			fail "could not identify generated patch $filename"
		printf '%s\t%s\t%s\t%s\t%s\n' \
			"$order" "$filename" "$patch_id" "$digest" "$subject"
		order=$((order + 1))
	done
} > "$manifest_tmp"

find "$SERIES_DIR" -maxdepth 1 -type f -name '*.patch' -delete
find "$temporary" -maxdepth 1 -type f -name '*.patch' \
	-exec mv {} "$SERIES_DIR" \;
mv -- "$manifest_tmp" "$MANIFEST"
"$POLICY" --line-root "$POLICY_ROOT" manifest >/dev/null
"$POLICY" --line-root "$POLICY_ROOT" topology \
	--source "$SOURCE" --base "$BASE_COMMIT" \
	--tip "$UML_KERNEL_PATCH_BRANCH" >/dev/null
"$POLICY" --line-root "$POLICY_ROOT" policy \
	--source "$SOURCE" --base "$BASE_COMMIT" \
	--tip "$UML_KERNEL_PATCH_BRANCH" >/dev/null
if [ -r "$LINE_ROOT/slices/x86_64/patches/metadata.env" ] &&
   [ -r "$LINE_ROOT/slices/x86_64/patches/series/manifest.tsv" ]; then
	python3 -B "$LINE_ROOT/scripts/update-slice-composition.py"
fi
printf '%s\n' "regenerated $UML_KERNEL_SLICE kernel patch series from rebased Git commits"
