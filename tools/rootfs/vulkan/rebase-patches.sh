#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

fail()
{
	printf '%s\n' "error: $*" >&2
	exit 1
}

usage()
{
	printf '%s\n' \
		"usage: $0 --component NAME --source TOPIC_CHECKOUT" >&2
	exit 2
}

COMPONENT=
SOURCE=
while [ "$#" -gt 0 ]; do
	case "$1" in
		--component)
			[ "$#" -ge 2 ] || usage
			COMPONENT=$2
			shift 2
			;;
		--source)
			[ "$#" -ge 2 ] || usage
			SOURCE=$2
			shift 2
			;;
		*) usage ;;
	esac
done
[ -n "$COMPONENT" ] && [ -n "$SOURCE" ] || usage

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
SOURCE=$(CDPATH= cd -- "$SOURCE" && pwd -P)
LOCK="$SCRIPT_DIR/sources.tsv"
[ -f "$LOCK" ] || fail "source lock is missing: $LOCK"
git -C "$SOURCE" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
	fail "source is not a Git checkout: $SOURCE"
[ -z "$(git -C "$SOURCE" status --porcelain=v1)" ] ||
	fail "topic checkout is dirty: $SOURCE"

tab=$(printf '\t')
row=$(awk -F '\t' -v component="$COMPONENT" '
	$1 == component { print; count++ }
	END { if (count != 1) exit 1 }
' "$LOCK") || fail "component is not uniquely pinned: $COMPONENT"
IFS="$tab" read -r locked_component repository base base_tree release_ref release_object series <<EOF
$row
EOF
[ "$locked_component" = "$COMPONENT" ] || fail "source-lock parser drift"
case "$repository" in https://*) ;; *) fail "repository is not HTTPS" ;; esac
case "$base" in
	*[!0-9a-f]*|'') fail "base commit is malformed: $base" ;;
esac
[ "${#base}" -eq 40 ] || fail "base commit is not full length: $base"
case "$base_tree" in
	*[!0-9a-f]*|'') fail "base tree is malformed: $base_tree" ;;
esac
[ "${#base_tree}" -eq 40 ] || fail "base tree is not full length: $base_tree"
[ "$(git -C "$SOURCE" rev-parse "$base^{tree}")" = "$base_tree" ] ||
	fail "base tree does not match source lock: $base_tree"
[ "$release_ref" = '-' ] || [ "$release_object" != '-' ] ||
	fail "release object is missing for $release_ref"
git -C "$SOURCE" cat-file -e "$base^{commit}" 2>/dev/null ||
	fail "base commit is absent from source: $base"
git -C "$SOURCE" merge-base --is-ancestor "$base" HEAD ||
	fail "topic does not descend from pinned base: $base"
[ -z "$(git -C "$SOURCE" rev-list --merges "$base..HEAD")" ] ||
	fail "topic contains a merge commit"

branch=$(git -C "$SOURCE" symbolic-ref --quiet --short HEAD) ||
	fail "topic checkout is detached"
GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
	git -C "$SOURCE" -c core.hooksPath=/dev/null rebase "$base"

commit_count=$(git -C "$SOURCE" rev-list --count "$base..HEAD")
[ "$commit_count" -gt 0 ] || fail "topic has no downstream commits"
series_path="$SCRIPT_DIR/$series"
series_directory=$(dirname -- "$series_path")
[ -d "$series_directory" ] || fail "patch directory is missing: $series_directory"
temporary=$(mktemp -d "$series_directory/.format-patch.XXXXXX")
cleanup()
{
	[ ! -d "${temporary:-}" ] || rm -rf -- "$temporary"
}
trap cleanup EXIT HUP INT TERM

git -C "$SOURCE" format-patch --no-signature \
	--output-directory "$temporary" "$base..HEAD" >/dev/null
generated_count=$(find "$temporary" -type f -name '*.patch' | wc -l | tr -d ' ')
[ "$generated_count" -eq "$commit_count" ] ||
	fail "Git emitted $generated_count patches for $commit_count commits"

manifest_tmp="$temporary/series.tsv"
{
	printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later'
	printf 'order\tpatch\tstable_patch_id\tsha256\tsubject\n'
	order=1
	find "$temporary" -type f -name '*.patch' -print | LC_ALL=C sort |
	while IFS= read -r patch; do
		filename=$(basename -- "$patch")
		patch_id=$(git patch-id --stable < "$patch" | awk 'NF { print $1; exit }')
		digest=$(shasum -a 256 "$patch" | awk '{ print $1 }')
		subject=$(git mailinfo /dev/null /dev/null < "$patch" |
			sed -n 's/^Subject: //p')
		[ -n "$patch_id" ] && [ -n "$subject" ] ||
			fail "could not identify generated patch: $filename"
		printf '%s\t%s\t%s\t%s\t%s\n' \
			"$order" "$filename" "$patch_id" "$digest" "$subject"
		order=$((order + 1))
	done
} > "$manifest_tmp"

find "$series_directory" -maxdepth 1 -type f -name '*.patch' -delete
find "$temporary" -maxdepth 1 -type f -name '*.patch' \
	-exec mv {} "$series_directory" \;
mv -- "$manifest_tmp" "$series_path"
printf '%s\n' "regenerated $COMPONENT patch series from $branch"
