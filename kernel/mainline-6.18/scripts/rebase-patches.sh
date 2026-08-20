#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Rebase the maintained kernel commits onto the pinned stable commit, then
# replace the checked-in mail series exclusively with git format-patch output.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
METADATA="$LINE_ROOT/patches/metadata.env"
SERIES_DIR="$LINE_ROOT/patches/series"
MANIFEST="$SERIES_DIR/manifest.tsv"
POLICY="$ROOT_DIR/kernel/common/tooling/verify-series-policy.py"
SOURCE_LOCATION="$LINE_ROOT/scripts/source-location.sh"
REQUESTED_SOURCE=

fail()
{
	printf '%s\n' "kernel/mainline-6.18/scripts/rebase-patches.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/mainline-6.18/scripts/rebase-patches.sh \
       [--source CONFIGURED_PATH]

PATH must be the clean maintained kernel topic branch. To modify a patch, use
`git rebase -i <UML_KERNEL_UPSTREAM_COMMIT>` in that checkout, amend/reorder the
topic commits, and then run this command. Never edit generated .patch files.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
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

[ -x "$SOURCE_LOCATION" ] || fail "missing source-location verifier: $SOURCE_LOCATION"
SOURCE=$("$SOURCE_LOCATION" --require-source) ||
	fail "kernel source configuration is invalid"
if [ -n "$REQUESTED_SOURCE" ] && [ "$REQUESTED_SOURCE" != "$SOURCE" ]; then
	fail "--source must match configured UML_KERNEL_SOURCE_DIR: $SOURCE"
fi
[ -r "$METADATA" ] || fail "missing metadata: $METADATA"
# shellcheck disable=SC1090
. "$METADATA"
[ "${UML_KERNEL_SERIES_SCHEMA:-}" = 3 ] || fail "unsupported series schema"
[ -d "$SOURCE/.git" ] || fail "source is not a Git working tree: $SOURCE"
[ -z "$(git -C "$SOURCE" status --porcelain --untracked-files=all)" ] ||
	fail "source working tree is dirty"
branch=$(git -C "$SOURCE" branch --show-current)
[ "$branch" = "$UML_KERNEL_PATCH_BRANCH" ] ||
	fail "source branch is '$branch', expected '$UML_KERNEL_PATCH_BRANCH'"
[ "$(git -C "$SOURCE" rev-parse "$UML_KERNEL_UPSTREAM_COMMIT^{commit}" 2>/dev/null || true)" = \
	"$UML_KERNEL_UPSTREAM_COMMIT" ] || fail "pinned upstream commit is absent"
git -C "$SOURCE" merge-base --is-ancestor "$UML_KERNEL_UPSTREAM_COMMIT" HEAD ||
	fail "maintained branch is not based on the pinned upstream commit"
[ -z "$(git -C "$SOURCE" rev-list --merges \
	"$UML_KERNEL_UPSTREAM_COMMIT..HEAD")" ] || fail "maintained branch has a merge"

# Git history is the editing authority. This is deliberately run even when it
# is a no-op, so series regeneration always follows an explicit rebase step.
GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
	git -C "$SOURCE" -c core.hooksPath=/dev/null \
		-c user.name='linux-um-darwin kernel patch maintainer' \
		-c user.email='noreply@linux-um-darwin.invalid' \
		rebase "$UML_KERNEL_UPSTREAM_COMMIT"

commit_count=$(git -C "$SOURCE" rev-list --count \
	"$UML_KERNEL_UPSTREAM_COMMIT..HEAD")
[ "$commit_count" -eq "$UML_KERNEL_PATCH_COUNT" ] ||
	fail "maintained branch has $commit_count commits, expected $UML_KERNEL_PATCH_COUNT"
[ -z "$(git -C "$SOURCE" rev-list --merges \
	"$UML_KERNEL_UPSTREAM_COMMIT..HEAD")" ] || fail "rebased branch has a merge"
final_tree=$(git -C "$SOURCE" rev-parse HEAD^{tree})
[ "$final_tree" = "$UML_KERNEL_FINAL_TREE" ] ||
	fail "rebased final tree is $final_tree, expected $UML_KERNEL_FINAL_TREE"

temporary=$(mktemp -d "$SERIES_DIR/.format-patch.XXXXXX")
cleanup()
{
	[ ! -d "${temporary:-}" ] || rm -rf -- "$temporary"
}
trap cleanup EXIT HUP INT TERM
git -C "$SOURCE" format-patch --numbered --no-signature \
	--output-directory "$temporary" \
	"$UML_KERNEL_UPSTREAM_COMMIT..HEAD" >/dev/null
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
"$POLICY" --line-root "$LINE_ROOT" manifest >/dev/null
"$POLICY" --line-root "$LINE_ROOT" topology \
	--source "$SOURCE" --base "$UML_KERNEL_UPSTREAM_COMMIT" \
	--tip "$UML_KERNEL_PATCH_BRANCH" >/dev/null
printf '%s\n' "regenerated kernel patch series from rebased Git commits"
