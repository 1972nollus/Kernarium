#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Export one maintained dependency topic checkout as its checked-in patch
# series and the tree it produces. Never edit the generated .patch files or
# series.tsv by hand.
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

PATH must be a clean Git checkout whose HEAD is the pinned upstream commit
followed by the dependency's topic commits. Amend, reorder, add, or drop
topics there (`git rebase -i <pinned-commit>`), then run this command to
export the series and record its tree.
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

row=$(awk -F '\t' -v wanted="$DEPENDENCY" '$1 == wanted { print; exit }' \
	"$MANIFEST")
[ -n "$row" ] || fail "$DEPENDENCY is absent from the manifest"
tab=$(printf '\t')
IFS="$tab" read -r name version products repository base source_paths series <<EOF
$row
EOF
[ "$name" = "$DEPENDENCY" ] || fail "could not parse dependency row"
[ "$series" != '-' ] || fail "$DEPENDENCY has no maintained patch series"
case "$series" in
rootfs-vulkan:*)
	fail "$DEPENDENCY is owned by tools/rootfs/vulkan/rebase-patches.sh"
	;;
*:*) fail "unknown patch-series owner: $series" ;;
esac

series_directory=$(dirname -- "$METADATA_ROOT/$series")
[ -d "$series_directory" ] || fail "patch directory is missing: $series_directory"
python3 -B "$ROOT_DIR/scripts/patch_series.py" export --source "$SOURCE" \
	--base "$base" --head HEAD --series-dir "$series_directory" --tree >/dev/null
printf '%s\n' "regenerated $DEPENDENCY patch series from $SOURCE"
