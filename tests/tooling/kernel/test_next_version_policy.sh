#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

REPOSITORY=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIER="$REPOSITORY/kernel/common/tooling/verify-series-policy.py"
WORK_DIRECTORY=$(mktemp -d "${TMPDIR:-/tmp}/uml-next-version-policy.XXXXXX")

cleanup()
{
	rm -rf -- "$WORK_DIRECTORY"
}
trap cleanup 0 HUP INT TERM

fail()
{
	printf '%s\n' "next-version policy fixture: $*" >&2
	exit 1
}

UPSTREAM="$WORK_DIRECTORY/upstream"
FIXTURE="$WORK_DIRECTORY/fixture"
mkdir -p "$UPSTREAM" "$FIXTURE/kernel/mainline-6.18/patches/series"
git -C "$UPSTREAM" init -q
git -C "$UPSTREAM" config user.name 'Yi Xie'
git -C "$UPSTREAM" config user.email 'imxieyi@users.noreply.github.com'
printf '%s\n' old > "$UPSTREAM/fix.txt"
printf '%s\n' old > "$UPSTREAM/host.txt"
printf '%s\n' old > "$UPSTREAM/port.txt"
git -C "$UPSTREAM" add fix.txt host.txt port.txt
git -C "$UPSTREAM" commit -q -m base
BASE=$(git -C "$UPSTREAM" rev-parse HEAD)

printf '%s\n' fixed > "$UPSTREAM/fix.txt"
printf '%s\n' portable > "$UPSTREAM/host.txt"
printf '%s\n' ported > "$UPSTREAM/port.txt"
git -C "$UPSTREAM" add fix.txt host.txt port.txt
git -C "$UPSTREAM" commit -q -m 'fixture policy topic'
FIX_COMMIT=$(git -C "$UPSTREAM" rev-parse HEAD)
PATCH="$FIXTURE/kernel/mainline-6.18/patches/series/0001-fixture-policy.patch"
git -C "$UPSTREAM" format-patch -1 --stdout "$FIX_COMMIT" > "$PATCH"

git -C "$UPSTREAM" switch -q -c conflict-base "$BASE"
printf '%s\n' independently-rewritten > "$UPSTREAM/fix.txt"
git -C "$UPSTREAM" add fix.txt
git -C "$UPSTREAM" commit -q -m 'conflicting base rewrite'
CONFLICT_COMMIT=$(git -C "$UPSTREAM" rev-parse HEAD)

printf '%s\n' 'UML_KERNEL_PATCH_COUNT=1' \
	> "$FIXTURE/kernel/mainline-6.18/patches/metadata.env"
patch_id=$(git patch-id --stable < "$PATCH" | awk '{print $1}')
digest=$(shasum -a 256 "$PATCH" | awk '{print $1}')
{
	printf 'order\tfilename\tstable_patch_id\tsha256\tsubject\n'
	printf '1\t%s\t%s\t%s\t%s\n' "$(basename -- "$PATCH")" \
		"$patch_id" "$digest" 'fixture policy topic'
} > "$FIXTURE/kernel/mainline-6.18/patches/series/manifest.tsv"
{
	printf 'pattern\towner\tdisposition\trationale\n'
	printf 'fix.txt\tbase-fix\tdrop-locally-if-contained\tcontained fix fixture\n'
	printf 'host.txt\thost-port\tcarry-locally\tlocal host fixture\n'
	printf 'port.txt\tarm64-port\tcarry-locally\tlocal port fixture\n'
} > "$FIXTURE/kernel/mainline-6.18/patches/path-ownership.tsv"
printf 'path\towner\tdecision\touter_boundary\tnext_action\trationale\n' \
	> "$FIXTURE/kernel/mainline-6.18/patches/extraction-decisions.tsv"
{
	printf 'path\towner\tdisposition\torigin_patch\tnext_version_policy\trationale\n'
	printf 'fix.txt\tbase-fix\tdrop-locally-if-contained\t0001\tdrop-locally-if-contained\tdrop fixture\n'
	printf 'host.txt\thost-port\tcarry-locally\t0001\tcarry-locally\tcarry fixture\n'
	printf 'port.txt\tarm64-port\tcarry-locally\t0001\tcarry-locally\tcarry fixture\n'
} > "$FIXTURE/kernel/mainline-6.18/patches/outside-port-justification.tsv"

assert_action()
{
	output=$1
	path=$2
	state=$3
	policy=$4
	action=$5
	awk -F '\t' -v path="$path" -v state="$state" \
		-v policy="$policy" -v action="$action" \
		'$2 == path && $3 == state && $4 == policy && $5 == action {
			found = 1
		} END { exit !found }' "$output" ||
		fail "missing $state action for $path ($policy -> $action)"
}

APPLIED="$WORK_DIRECTORY/applied"
git clone -q "$UPSTREAM" "$APPLIED"
git -C "$APPLIED" switch -q --detach "$BASE"
"$VERIFIER" --line-root "$FIXTURE/kernel/mainline-6.18" --source "$APPLIED" --base HEAD \
	next-version > "$WORK_DIRECTORY/applied.out"
assert_action "$WORK_DIRECTORY/applied.out" fix.txt applied \
	outside/drop-locally-if-contained \
	carry-locally-until-contained-and-retest
assert_action "$WORK_DIRECTORY/applied.out" host.txt applied \
	outside/carry-locally carry-locally-and-retest
assert_action "$WORK_DIRECTORY/applied.out" port.txt applied \
	outside/carry-locally carry-locally-and-retest

CONTAINED="$WORK_DIRECTORY/contained"
git clone -q "$UPSTREAM" "$CONTAINED"
git -C "$CONTAINED" switch -q --detach "$FIX_COMMIT"
if "$VERIFIER" --line-root "$FIXTURE/kernel/mainline-6.18" --source "$CONTAINED" --base HEAD \
	next-version > "$WORK_DIRECTORY/contained.out" \
	2> "$WORK_DIRECTORY/contained.err"; then
	fail 'already-contained topic was accepted'
fi
grep -F 'is already contained' "$WORK_DIRECTORY/contained.err" >/dev/null ||
	fail 'already-contained topic was reported as a generic conflict'
assert_action "$WORK_DIRECTORY/contained.out" fix.txt contained \
	outside/drop-locally-if-contained drop-locally-and-retest
assert_action "$WORK_DIRECTORY/contained.out" host.txt contained \
	outside/carry-locally \
	review-contained-change-before-carrying-locally-and-retest
assert_action "$WORK_DIRECTORY/contained.out" port.txt contained \
	outside/carry-locally \
	review-contained-change-before-carrying-locally-and-retest

CONFLICT="$WORK_DIRECTORY/conflict"
git clone -q "$UPSTREAM" "$CONFLICT"
git -C "$CONFLICT" switch -q --detach "$CONFLICT_COMMIT"
if "$VERIFIER" --line-root "$FIXTURE/kernel/mainline-6.18" --source "$CONFLICT" --base HEAD \
	next-version > "$WORK_DIRECTORY/conflict.out" \
	2> "$WORK_DIRECTORY/conflict.err"; then
	fail 'genuine patch conflict was accepted'
fi
grep -F 'genuinely conflicts' "$WORK_DIRECTORY/conflict.err" >/dev/null ||
	fail 'genuine conflict was not distinguished from containment'
assert_action "$WORK_DIRECTORY/conflict.out" fix.txt conflict \
	outside/drop-locally-if-contained \
	check-for-equivalent-base-change-then-drop-locally-or-rebase-locally-and-retest
assert_action "$WORK_DIRECTORY/conflict.out" host.txt conflict \
	outside/carry-locally rebase-locally-and-retest
assert_action "$WORK_DIRECTORY/conflict.out" port.txt conflict \
	outside/carry-locally rebase-locally-and-retest

printf '%s\n' 'Linux next-version policy action fixtures passed'
