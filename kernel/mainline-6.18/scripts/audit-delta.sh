#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
COMMON_ROOT="$ROOT_DIR/kernel/common"
SERIES_POLICY="$COMMON_ROOT/tooling/verify-series-policy.py"
SOURCE_LOCATION="$LINE_ROOT/scripts/source-location.sh"
SLICE_SELECTOR="$LINE_ROOT/scripts/select-slice.sh"

fail()
{
	printf '%s\n' "kernel/mainline-6.18/scripts/audit-delta.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/mainline-6.18/scripts/audit-delta.sh \
       [--slice arm64|x86_64] [--source CONFIGURED_PATH] [--tree TREE]

Classify every path changed by the selected common or x86_64 overlay series and
enforce its checked-in file-count ceilings. TREE defaults to HEAD.
EOF
}

REQUESTED_SOURCE=''
TREE=''
SLICE=arm64
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

[ -r "$SLICE_SELECTOR" ] || fail "missing slice selector: $SLICE_SELECTOR"
# shellcheck disable=SC1090
. "$SLICE_SELECTOR"
uml_kernel_select_slice "$LINE_ROOT" "$SLICE" ||
	fail "cannot select kernel slice: $SLICE"
METADATA=$UML_KERNEL_METADATA_PATH
OWNERSHIP="$UML_KERNEL_POLICY_ROOT/patches/path-ownership.tsv"
OUTSIDE_POLICY="$UML_KERNEL_POLICY_ROOT/patches/outside-port-justification.tsv"
SERIES_MANIFEST=$UML_KERNEL_MANIFEST_PATH
SERIES_DIR=$UML_KERNEL_PATCH_DIR
POLICY_ROOT=$UML_KERNEL_POLICY_ROOT

[ -x "$SOURCE_LOCATION" ] || fail "missing source-location verifier: $SOURCE_LOCATION"
SOURCE=$("$SOURCE_LOCATION" --require-source) ||
	fail "kernel source configuration is invalid"
if [ -n "$REQUESTED_SOURCE" ] && [ "$REQUESTED_SOURCE" != "$SOURCE" ]; then
	fail "--source must match configured UML_KERNEL_SOURCE_DIR: $SOURCE"
fi
[ -r "$METADATA" ] || fail "missing metadata: $METADATA"
[ -r "$OWNERSHIP" ] || fail "missing ownership manifest: $OWNERSHIP"
[ -r "$OUTSIDE_POLICY" ] || fail "missing outside-port policy: $OUTSIDE_POLICY"
[ -r "$SERIES_MANIFEST" ] || fail "missing series manifest: $SERIES_MANIFEST"
[ -x "$SERIES_POLICY" ] || fail "missing series policy verifier: $SERIES_POLICY"
"$SERIES_POLICY" --line-root "$POLICY_ROOT" manifest >/dev/null
[ -d "$SOURCE/.git" ] || fail "not a Linux checkout: $SOURCE"
TREE=${TREE:-HEAD}
git -C "$SOURCE" show-ref --verify --quiet "$UML_KERNEL_BASE_REF" ||
	fail "pinned upstream base ref is unavailable"
upstream_commit=$(git -C "$SOURCE" rev-parse "$UML_KERNEL_BASE_REF^{commit}")
[ "$upstream_commit" = "$UML_KERNEL_UPSTREAM_COMMIT" ] ||
	fail "upstream base is $upstream_commit, expected $UML_KERNEL_UPSTREAM_COMMIT"
base_tree=$(git -C "$SOURCE" rev-parse "$UML_KERNEL_BASE_REF^{tree}")
[ "$base_tree" = "$UML_KERNEL_UPSTREAM_TREE" ] ||
	fail "upstream base tree is $base_tree, expected $UML_KERNEL_UPSTREAM_TREE"
BASE_COMMIT=$upstream_commit
if [ "$UML_KERNEL_SLICE" = x86_64 ]; then
	common_tree=$(git -C "$SOURCE" rev-parse \
		"$UML_KERNEL_COMMON_PATCH_BRANCH^{tree}" 2>/dev/null || true)
	[ "$common_tree" = "$UML_KERNEL_COMMON_FINAL_TREE" ] ||
		fail "x86_64 parent is not the canonical common kernel tree"
	BASE_COMMIT=$(git -C "$SOURCE" rev-parse \
		"$UML_KERNEL_COMMON_PATCH_BRANCH^{commit}")
fi
git -C "$SOURCE" cat-file -e "$TREE^{tree}" 2>/dev/null ||
	fail "tree is unavailable: $TREE"
git -C "$SOURCE" merge-base --is-ancestor "$BASE_COMMIT" "$TREE" ||
	fail "selected tree is not based on the $UML_KERNEL_SLICE series parent"

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
git -C "$SOURCE" diff --numstat "$BASE_COMMIT" "$TREE" \
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

# Build an exact path-to-topic index from the checked-in series. The manifest
# verifier above rejects every undeclared multi-topic path and requires each
# declared overlap to name its exact ordered owners. This retains the
# change-then-revert guard while permitting a small independently droppable fix
# to share a path with its broader subsystem topic.
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
	carry-locally:carry-locally|drop-locally-if-contained:drop-locally-if-contained) ;;
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
