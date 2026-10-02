#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
PUBLISHER="$ROOT_DIR/dependencies/publish-prefix.py"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-publish-prefix.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test-publish-prefix.sh
. "$(dirname -- "$0")/../support/expect.sh"

publish_status()
{
	status=0
	python3 "$PUBLISHER" "$@" >"$TEST_ROOT/publish.out" \
		2>"$TEST_ROOT/publish.err" || status=$?
	printf '%s\n' "$status"
}

staged()
{
	mkdir -p "$TEST_ROOT/$1"
	printf '%s\n' "$1" >"$TEST_ROOT/$1/identity"
}

identity()
{
	cat "$TEST_ROOT/$1/identity"
}

# An exclusive rename publishes a stage whose destination is absent, from
# any directory on the same filesystem.
staged exclusive-stage
mkdir -p "$TEST_ROOT/published"
[ "$(publish_status "$TEST_ROOT/exclusive-stage" \
	"$TEST_ROOT/published/exclusive")" = 0 ] ||
	fail "exclusive publication of an absent destination failed"
[ "$(identity published/exclusive)" = exclusive-stage ] ||
	fail "exclusive publication moved the wrong directory"
[ ! -e "$TEST_ROOT/exclusive-stage" ] ||
	fail "exclusive publication left its stage behind"

# A destination that already exists is reported with status 1 and neither
# directory changes, so a caller can verify and reuse the winner.
staged loser
[ "$(publish_status "$TEST_ROOT/loser" "$TEST_ROOT/published/exclusive")" = 1 ] ||
	fail "an existing destination did not report status 1"
[ ! -s "$TEST_ROOT/publish.err" ] ||
	fail "an existing destination printed a diagnostic"
[ "$(identity published/exclusive)" = exclusive-stage ] ||
	fail "an existing destination was replaced"
[ ! -e "$TEST_ROOT/published/exclusive/loser" ] ||
	fail "a losing stage was moved below the destination"
[ "$(identity loser)" = loser ] || fail "a losing stage was modified"

ln -s loser "$TEST_ROOT/published/link"
[ "$(publish_status "$TEST_ROOT/loser" "$TEST_ROOT/published/link")" = 1 ] ||
	fail "a symlinked destination did not report status 1"

# A swap exchanges a replacement with the directory it replaces.
staged replacement
[ "$(publish_status --swap "$TEST_ROOT/replacement" \
	"$TEST_ROOT/published/exclusive")" = 0 ] || fail "swap failed"
[ "$(identity published/exclusive)" = replacement ] ||
	fail "swap did not publish the replacement"
[ "$(identity replacement)" = exclusive-stage ] ||
	fail "swap did not return the replaced directory"

# Every other failure is status 2 with a diagnostic.
for case in \
	"--swap $TEST_ROOT/loser $TEST_ROOT/published/absent" \
	"$TEST_ROOT/absent $TEST_ROOT/published/other" \
	"$TEST_ROOT/published/link $TEST_ROOT/published/other" \
	"$TEST_ROOT/loser" \
	"--exchange $TEST_ROOT/loser $TEST_ROOT/published/other"; do
	# shellcheck disable=SC2086
	[ "$(publish_status $case)" = 2 ] || fail "accepted: $case"
	[ -s "$TEST_ROOT/publish.err" ] || fail "no diagnostic for: $case"
done
[ "$(identity loser)" = loser ] || fail "a rejected request changed its source"

# Two concurrent publications of one destination leave exactly one winner
# and one untouched loser.
PUBLISH_ROOT="$TEST_ROOT/concurrent-publication"
mkdir -p "$PUBLISH_ROOT/first" "$PUBLISH_ROOT/second"
printf '%s\n' first >"$PUBLISH_ROOT/first/identity"
printf '%s\n' second >"$PUBLISH_ROOT/second/identity"
python3 "$PUBLISHER" "$PUBLISH_ROOT/first" "$PUBLISH_ROOT/winner" &
FIRST_PID=$!
python3 "$PUBLISHER" "$PUBLISH_ROOT/second" "$PUBLISH_ROOT/winner" &
SECOND_PID=$!
FIRST_STATUS=0
SECOND_STATUS=0
wait "$FIRST_PID" || FIRST_STATUS=$?
wait "$SECOND_PID" || SECOND_STATUS=$?
case "$FIRST_STATUS:$SECOND_STATUS" in
0:1|1:0) ;;
*) fail "exclusive publication returned $FIRST_STATUS and $SECOND_STATUS" ;;
esac
[ -f "$PUBLISH_ROOT/winner/identity" ] ||
	fail "concurrent publication produced no winner"
LOSERS=$(find "$PUBLISH_ROOT" -mindepth 1 -maxdepth 1 -type d \
	\( -name first -o -name second \) | wc -l | tr -d ' ')
[ "$LOSERS" -eq 1 ] || fail "concurrent publication did not retain one loser"
[ -z "$(find "$PUBLISH_ROOT/winner" -mindepth 1 -type d)" ] ||
	fail "concurrent publication nested the loser in the winner"

# The builders' shared publish step verifies the stage, publishes it, reuses
# a concurrent winner only after verifying it, and fails on anything else.
HELPER_ROOT="$TEST_ROOT/publish-verified"
mkdir -p "$HELPER_ROOT"
cat >"$HELPER_ROOT/run.sh" <<'EOF_RUN'
set -eu
ROOT_DIR=$1
PYTHON=python3
fail()
{
	printf '%s\n' "$*" >&2
	exit 1
}
verify_fixture()
{
	[ -f "$1/identity" ] && [ ! -e "$1/invalid" ]
}
. "$ROOT_DIR/dependencies/build-common.sh"
STAGE=$2
publish_verified "$2" "$3" verify_fixture "fixture prefix"
printf 'stage=%s\n' "$STAGE"
EOF_RUN

publish_verified_status()
{
	status=0
	sh "$HELPER_ROOT/run.sh" "$ROOT_DIR" "$1" "$2" \
		>"$HELPER_ROOT/out" 2>"$HELPER_ROOT/err" || status=$?
	printf '%s\n' "$status"
}

staged helper-first
[ "$(publish_verified_status "$TEST_ROOT/helper-first" \
	"$HELPER_ROOT/prefix")" = 0 ] || fail "publish_verified did not publish"
grep -qx "published fixture prefix: $HELPER_ROOT/prefix" "$HELPER_ROOT/out" ||
	fail "publish_verified did not report its publication"
grep -qx 'stage=' "$HELPER_ROOT/out" ||
	fail "publish_verified kept STAGE after publishing it"
[ "$(identity publish-verified/prefix)" = helper-first ] ||
	fail "publish_verified published the wrong directory"

staged helper-second
[ "$(publish_verified_status "$TEST_ROOT/helper-second" \
	"$HELPER_ROOT/prefix")" = 0 ] || fail "publish_verified did not reuse a winner"
grep -qx "reusing concurrently published fixture prefix: $HELPER_ROOT/prefix" \
	"$HELPER_ROOT/out" || fail "publish_verified did not report the reuse"
grep -qx "stage=$TEST_ROOT/helper-second" "$HELPER_ROOT/out" ||
	fail "publish_verified cleared STAGE for a stage it did not publish"
[ "$(identity helper-second)" = helper-second ] ||
	fail "publish_verified changed a losing stage"

: >"$HELPER_ROOT/prefix/invalid"
[ "$(publish_verified_status "$TEST_ROOT/helper-second" \
	"$HELPER_ROOT/prefix")" = 1 ] || fail "publish_verified reused an invalid winner"
grep -q 'concurrently published fixture prefix is invalid' "$HELPER_ROOT/err" ||
	fail "an invalid winner had no diagnostic"

: >"$TEST_ROOT/helper-second/invalid"
[ "$(publish_verified_status "$TEST_ROOT/helper-second" \
	"$HELPER_ROOT/fresh")" != 0 ] || fail "publish_verified accepted an invalid stage"
[ ! -e "$HELPER_ROOT/fresh" ] || fail "publish_verified published an invalid stage"
rm -f "$TEST_ROOT/helper-second/invalid"

[ "$(publish_verified_status "$TEST_ROOT/helper-second" \
	"$HELPER_ROOT/absent/prefix")" = 1 ] ||
	fail "publish_verified accepted a publisher failure"
grep -q "could not publish fixture prefix: $HELPER_ROOT/absent/prefix" \
	"$HELPER_ROOT/err" || fail "a publisher failure had no diagnostic"
[ "$(identity helper-second)" = helper-second ] ||
	fail "a publisher failure moved its stage"

printf '%s\n' "exclusive, swap and verified publication tests passed"
