#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
PUBLISHER="$ROOT_DIR/dependencies/publish-prefix.py"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-publish-prefix.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

fail()
{
	printf '%s\n' "test-publish-prefix.sh: $*" >&2
	exit 1
}

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

printf '%s\n' "exclusive and swap publication tests passed"
