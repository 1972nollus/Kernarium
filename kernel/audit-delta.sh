#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
METADATA="$ROOT_DIR/kernel/patches/metadata.env"
OWNERSHIP="$ROOT_DIR/kernel/patches/path-ownership.tsv"
OUTSIDE_POLICY="$ROOT_DIR/kernel/patches/outside-port-justification.tsv"
SERIES_MANIFEST="$ROOT_DIR/kernel/patches/series/manifest.tsv"
SERIES_DIR="$ROOT_DIR/kernel/patches/series"

fail()
{
	printf '%s\n' "kernel/audit-delta.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/audit-delta.sh --source PATH [--tree TREE]

Classify every path changed from the authenticated packaged Linux base and
enforce the checked-in file-count ceilings. TREE defaults to HEAD.
EOF
}

SOURCE=''
TREE=''
while [ "$#" -gt 0 ]; do
	case "$1" in
	--source)
		[ "$#" -ge 2 ] || fail "--source requires a path"
		SOURCE=$2
		shift 2
		;;
	--tree)
		[ "$#" -ge 2 ] || fail "--tree requires an object name"
		TREE=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$SOURCE" ] || fail "--source is required"
[ -r "$METADATA" ] || fail "missing metadata: $METADATA"
[ -r "$OWNERSHIP" ] || fail "missing ownership manifest: $OWNERSHIP"
[ -r "$OUTSIDE_POLICY" ] || fail "missing outside-port policy: $OUTSIDE_POLICY"
[ -r "$SERIES_MANIFEST" ] || fail "missing series manifest: $SERIES_MANIFEST"
# shellcheck disable=SC1090
. "$METADATA"
[ -d "$SOURCE/.git" ] || fail "not a Linux checkout: $SOURCE"
TREE=${TREE:-HEAD}
git -C "$SOURCE" show-ref --verify --quiet "$UML_KERNEL_BASE_REF" ||
	fail "authenticated packaged base ref is unavailable"
base_tree=$(git -C "$SOURCE" rev-parse "$UML_KERNEL_BASE_REF^{tree}")
[ "$base_tree" = "$UML_KERNEL_PACKAGED_TREE" ] ||
	fail "packaged base tree is $base_tree, expected $UML_KERNEL_PACKAGED_TREE"
git -C "$SOURCE" cat-file -e "$TREE^{tree}" 2>/dev/null ||
	fail "tree is unavailable: $TREE"

DELTA_TMP=$(/usr/bin/mktemp -d /private/tmp/uml-delta-audit.XXXXXX)
cleanup()
{
	rm -f -- "$DELTA_TMP/paths.tsv" "$DELTA_TMP/classified.tsv" \
		"$DELTA_TMP/outside-paths.txt" \
		"$DELTA_TMP/series-membership.tsv" \
		"$DELTA_TMP/policy-paths.txt" "$DELTA_TMP"/patch-*.tsv
	rmdir -- "$DELTA_TMP" 2>/dev/null || true
}
trap cleanup 0 HUP INT TERM
: > "$DELTA_TMP/outside-paths.txt"
: > "$DELTA_TMP/series-membership.tsv"
: > "$DELTA_TMP/policy-paths.txt"
git -C "$SOURCE" diff --numstat "$UML_KERNEL_BASE_REF" "$TREE" \
	--output="$DELTA_TMP/paths.tsv"

tab=$(printf '\t')

classify_path()
{
	classified_path=$1
	CLASSIFIED_OWNER=''
	CLASSIFIED_DISPOSITION=''
	while IFS="$tab" read -r pattern candidate_owner \
		candidate_disposition rationale; do
		case "$pattern" in \#*|pattern|'') continue ;; esac
		case "$classified_path" in
		$pattern)
			CLASSIFIED_OWNER=$candidate_owner
			CLASSIFIED_DISPOSITION=$candidate_disposition
			break
			;;
		esac
	done < "$OWNERSHIP"
}

# Build an exact path-to-topic index from the checked-in series. A final-state
# topic series must touch every replay delta path in exactly one patch; this
# rejects reintroduced broad patches and change-then-revert churn.
while IFS="$tab" read -r order filename patch_id digest subject; do
	case "$order" in \#*|order|'') continue ;; esac
	case "$order:$filename" in
	*[!0-9]*:*|*:/*|*:..*) fail "invalid series manifest row: $order $filename" ;;
	esac
	patch="$SERIES_DIR/$filename"
	[ -f "$patch" ] || fail "series patch is missing: $patch"
	origin=$(printf '%04d' "$order")
	patch_numstat="$DELTA_TMP/patch-$origin.tsv"
	# git apply filters paths relative to the caller's current subdirectory.
	# Anchor parsing at the repository root so CMake execute_process() and a
	# direct shell invocation inspect the same complete topic patch.
	git -C "$ROOT_DIR" apply --numstat "$patch" > "$patch_numstat" ||
		fail "cannot inspect topic patch paths: $patch"
	while IFS="$tab" read -r added removed path; do
		[ -n "$path" ] || continue
		printf '%s\t%s\n' "$origin" "$path" \
			>> "$DELTA_TMP/series-membership.tsv"
	done < "$patch_numstat"
done < "$SERIES_MANIFEST"

duplicate_series_path=$(awk -F '\t' '
	{ count[$2]++ }
	END { for (path in count) if (count[path] != 1) { print path; exit } }
' "$DELTA_TMP/series-membership.tsv")
[ -z "$duplicate_series_path" ] ||
	fail "series path is changed by more than one topic patch: $duplicate_series_path"

# Validate policy semantics independently of the current tree walk so an
# owner, disposition, origin, or next-version typo cannot become accepted just
# because every field is nonempty.
while IFS="$tab" read -r policy_path policy_owner policy_disposition \
	origin_patch next_version_policy rationale; do
	case "$policy_path" in \#*|path|'') continue ;; esac
	grep -F -x -- "$policy_path" "$DELTA_TMP/policy-paths.txt" >/dev/null &&
		fail "duplicate outside-port policy path: $policy_path"
	printf '%s\n' "$policy_path" >> "$DELTA_TMP/policy-paths.txt"
	classify_path "$policy_path"
	[ -n "$CLASSIFIED_OWNER" ] ||
		fail "outside-port policy path has no ownership rule: $policy_path"
	[ "$policy_owner" = "$CLASSIFIED_OWNER" ] ||
		fail "outside-port owner mismatch for $policy_path: $policy_owner, expected $CLASSIFIED_OWNER"
	[ "$policy_disposition" = "$CLASSIFIED_DISPOSITION" ] ||
		fail "outside-port disposition mismatch for $policy_path: $policy_disposition, expected $CLASSIFIED_DISPOSITION"
	case "$policy_disposition:$next_version_policy" in
	keep:carry|upstream:retest-and-upstream|upstream:drop-when-base-contains-fix) ;;
	*) fail "invalid next-version policy for $policy_path: $policy_disposition/$next_version_policy" ;;
	esac
	case "$origin_patch" in
	[0-9][0-9][0-9][0-9]) ;;
	*) fail "invalid origin patch for $policy_path: $origin_patch" ;;
	esac
	series_origin=$(awk -F '\t' -v path="$policy_path" \
		'$2 == path { print $1; exit }' "$DELTA_TMP/series-membership.tsv")
	[ "$series_origin" = "$origin_patch" ] ||
		fail "outside-port origin mismatch for $policy_path: $origin_patch, actual ${series_origin:-none}"
	[ -n "$rationale" ] || fail "outside-port rationale is empty: $policy_path"
done < "$OUTSIDE_POLICY"

# A topic patch may not carry a path which disappears from the final replay.
while IFS="$tab" read -r origin_patch series_path; do
	awk -F '\t' -v path="$series_path" '$3 == path { found = 1 }
		END { exit !found }' "$DELTA_TMP/paths.tsv" ||
		fail "series path is absent from the final replay delta: $series_path"
done < "$DELTA_TMP/series-membership.tsv"

changed=0
outside=0
while IFS="$tab" read -r added removed changed_path; do
	[ -n "$changed_path" ] || continue
	classify_path "$changed_path"
	owner=$CLASSIFIED_OWNER
	disposition=$CLASSIFIED_DISPOSITION
	[ -n "$owner" ] || fail "unclassified Linux delta path: $changed_path"
	series_origin=$(awk -F '\t' -v path="$changed_path" \
		'$2 == path { print $1; exit }' "$DELTA_TMP/series-membership.tsv")
	[ -n "$series_origin" ] ||
		fail "final replay path is absent from the topic series: $changed_path"
	printf '%s\t%s\t%s\t%s\t%s\n' \
		"$owner" "$disposition" "$added" "$removed" "$changed_path" \
		>> "$DELTA_TMP/classified.tsv"
	changed=$((changed + 1))
	case "$changed_path" in
	arch/arm64/um/*|arch/um/*) ;;
	*)
		justified=''
		while IFS="$tab" read -r policy_path policy_owner \
			policy_disposition origin_patch next_version_policy rationale; do
			case "$policy_path" in \#*|path|'') continue ;; esac
			if [ "$policy_path" = "$changed_path" ] &&
			   [ "$policy_owner" = "$owner" ] &&
			   [ "$policy_disposition" = "$disposition" ] &&
			   [ "$origin_patch" = "$series_origin" ]; then
				justified=1
				break
			fi
		done < "$OUTSIDE_POLICY"
		[ -n "$justified" ] ||
			fail "outside-port delta lacks an exact justification: $changed_path"
		printf '%s\n' "$changed_path" >> "$DELTA_TMP/outside-paths.txt"
		outside=$((outside + 1))
		;;
	esac
done < "$DELTA_TMP/paths.tsv"

while IFS="$tab" read -r policy_path policy_owner policy_disposition \
	origin_patch next_version_policy rationale; do
	case "$policy_path" in \#*|path|'') continue ;; esac
	grep -F -x -- "$policy_path" "$DELTA_TMP/outside-paths.txt" >/dev/null ||
		fail "stale outside-port justification has no replay delta: $policy_path"
done < "$OUTSIDE_POLICY"

[ "$changed" -le "$UML_KERNEL_MAX_CHANGED_FILES" ] ||
	fail "delta grew to $changed files (ceiling $UML_KERNEL_MAX_CHANGED_FILES)"
[ "$outside" -le "$UML_KERNEL_MAX_OUTSIDE_PORT_FILES" ] ||
	fail "outside-port delta grew to $outside files (ceiling $UML_KERNEL_MAX_OUTSIDE_PORT_FILES)"

printf 'owner\tdisposition\tfiles\tadded\tremoved\n'
awk -F '\t' '
{
	key = $1 FS $2
	files[key]++
	added[key] += $3
	removed[key] += $4
}
END {
	for (key in files)
		print key FS files[key] FS added[key] FS removed[key]
}' "$DELTA_TMP/classified.tsv" | LC_ALL=C sort
printf 'total\tall\t%d\t-\t-\n' "$changed"
printf 'outside-port\tall\t%d\t-\t-\n' "$outside"
