#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Rebase one maintained dependency topic branch onto its pinned upstream commit,
# then replace its checked-in patch series exclusively with git format-patch
# output. Never edit the generated .patch files by hand.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MANIFEST=${UML_DEPENDENCY_MANIFEST:-"$ROOT_DIR/dependencies/manifest.tsv"}
METADATA_ROOT=${UML_DEPENDENCY_METADATA_ROOT:-"$ROOT_DIR/dependencies"}
DEPENDENCY=
SOURCE=

fail()
{
	printf '%s\n' "dependencies/rebase-patches.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: dependencies/rebase-patches.sh --dependency NAME --source PATH

PATH must be a clean Git topic branch whose history is the pinned upstream
commit followed by the dependency's maintained commits. To edit a topic, first
use `git rebase -i <pinned-commit>` in PATH, amend or reorder commits there,
then run this command to regenerate the checked-in mail patch series.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--dependency)
		[ "$#" -ge 2 ] || fail "--dependency requires a name"
		DEPENDENCY=$2
		shift 2
		;;
	--source)
		[ "$#" -ge 2 ] || fail "--source requires a path"
		SOURCE=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$DEPENDENCY" ] && [ -n "$SOURCE" ] || {
	usage >&2
	exit 1
}
case "$DEPENDENCY" in
*[!A-Za-z0-9._+-]*|'') fail "unsafe dependency name: $DEPENDENCY" ;;
esac
[ -f "$MANIFEST" ] || fail "manifest is missing: $MANIFEST"
[ -d "$SOURCE/.git" ] || fail "source is not a Git working tree: $SOURCE"
[ -z "$(git -C "$SOURCE" status --porcelain --untracked-files=all)" ] ||
	fail "source working tree is dirty"
branch=$(git -C "$SOURCE" symbolic-ref --quiet --short HEAD 2>/dev/null || true)
[ -n "$branch" ] || fail "source must have a checked-out topic branch"

row=$(awk -F '\t' -v wanted="$DEPENDENCY" '$1 == wanted { print; exit }' \
	"$MANIFEST")
[ -n "$row" ] || fail "$DEPENDENCY is absent from the manifest"
tab=$(printf '\t')
IFS="$tab" read -r name version products repository base source_paths series <<EOF
$row
EOF
[ "$name" = "$DEPENDENCY" ] || fail "could not parse dependency row"
[ "$series" != '-' ] || fail "$DEPENDENCY has no maintained patch series"
[ "$(git -C "$SOURCE" rev-parse "$base^{commit}" 2>/dev/null || true)" = "$base" ] ||
	fail "pinned upstream commit is absent from source: $base"
git -C "$SOURCE" merge-base --is-ancestor "$base" HEAD ||
	fail "topic branch is not based on pinned upstream commit $base"
[ -z "$(git -C "$SOURCE" rev-list --merges "$base..HEAD")" ] ||
	fail "topic branch contains a merge commit"

# Even a no-op rebase is deliberate: it makes Git, rather than patch-file
# editing, the authority for how the topic relates to the pinned upstream.
GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
	git -C "$SOURCE" -c core.hooksPath=/dev/null rebase "$base"

commit_count=$(git -C "$SOURCE" rev-list --count "$base..HEAD")
[ "$commit_count" -gt 0 ] || fail "topic branch contains no maintained commits"
[ -z "$(git -C "$SOURCE" rev-list --merges "$base..HEAD")" ] ||
	fail "rebased topic branch contains a merge commit"

series_path="$METADATA_ROOT/$series"
series_directory=$(dirname -- "$series_path")
[ -d "$series_directory" ] || fail "patch directory is missing: $series_directory"
temporary=$(mktemp -d "$series_directory/.format-patch.XXXXXX")
cleanup()
{
	[ ! -d "${temporary:-}" ] || rm -rf -- "$temporary"
}
trap cleanup EXIT HUP INT TERM

git -C "$SOURCE" format-patch --no-signature --output-directory "$temporary" \
	"$base..HEAD" >/dev/null
generated_count=$(find "$temporary" -type f -name '*.patch' | wc -l | tr -d ' ')
[ "$generated_count" -eq "$commit_count" ] ||
	fail "git format-patch emitted $generated_count files for $commit_count commits"

manifest_tmp="$temporary/series.tsv"
{
	printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later'
	printf 'order\tpatch\tstable_patch_id\tsha256\tsubject\n'
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

find "$series_directory" -maxdepth 1 -type f -name '*.patch' -delete
find "$temporary" -maxdepth 1 -type f -name '*.patch' -exec mv {} "$series_directory" \;
mv -- "$manifest_tmp" "$series_path"
printf '%s\n' "regenerated $DEPENDENCY patch series from $branch"
