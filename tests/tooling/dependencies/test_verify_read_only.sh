#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Every dependency verification mode is read-only: verifying an absent prefix
# fails with a diagnostic and leaves the filesystem exactly as it was.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
DEPENDENCIES="$ROOT_DIR/dependencies"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-verify-read-only.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test-verify-read-only.sh
. "$(dirname -- "$0")/../support/expect.sh"

# Every path a verifier could resolve lies below SCRATCH, whose entries and
# modification times are compared around each run, so a directory created
# and removed again is also caught.
SCRATCH="$TEST_ROOT/scratch"
SOURCES="$SCRATCH/sources"
CACHE="$SCRATCH/cache"
WORK="$SCRATCH/work"
mkdir -p "$SCRATCH/parent"

snapshot()
{
	find "$SCRATCH" -exec stat -f '%Fm %N' {} + | LC_ALL=C sort
}

expect_unchanged()
{
	after=$(snapshot)
	[ "$after" = "$1" ] || {
		printf '%s\n' "before:" "$1" "after:" "$after" >&2
		fail "$2 changed the filesystem"
	}
}

# expect_read_only_failure DESCRIPTION COMMAND...: COMMAND must fail with a
# diagnostic and write nothing.
expect_read_only_failure()
{
	before=$(snapshot)
	expect_failure "$@"
	expect_unchanged "$before" "$1"
}

# A missing prefix whose parent exists, and one whose parent is missing too.
for PREFIX in "$SCRATCH/parent/absent" "$SCRATCH/absent-parent/prefix"; do
	expect_read_only_failure "iOS network verification of $PREFIX" \
		"$DEPENDENCIES/build-ios-network.sh" --verify-only \
		--prefix "$PREFIX" --source-root "$SOURCES" --work-root "$WORK"
	expect_read_only_failure "Apple GPU verification of $PREFIX" \
		"$DEPENDENCIES/build-apple-gpu.sh" --verify-only \
		--prefix "$PREFIX" --source-root "$SOURCES" --work-root "$WORK"
	expect_read_only_failure "iOS storage verification of $PREFIX" \
		"$DEPENDENCIES/build-ios-storage.sh" --verify-only \
		--output "$PREFIX" --source-root "$SOURCES" --work-root "$WORK"
	expect_read_only_failure "iOS zstd verification of $PREFIX" \
		"$DEPENDENCIES/build-ios-zstd.sh" --verify-only \
		--output "$PREFIX" --source-root "$SOURCES" --work-root "$WORK"
	expect_read_only_failure "kernel host tools verification of $PREFIX" \
		"$DEPENDENCIES/build-kernel-host-tools.sh" --verify-only \
		--output "$PREFIX" --source-root "$SOURCES" --cache "$CACHE" \
		--work-root "$WORK"
	expect_read_only_failure "macOS prefix verification of $PREFIX" \
		"$DEPENDENCIES/verify-prefix.sh" --prefix "$PREFIX" \
		--manifest "$DEPENDENCIES/manifest.tsv" --deployment-target 14.0
	expect_read_only_failure "runtime closure verification of $PREFIX" \
		"$DEPENDENCIES/verify-runtime-dependencies.sh" \
		--binary "$PREFIX/uml-darwin" --apple-gpu-prefix "$PREFIX" \
		--framework-directory "$PREFIX/Frameworks"
	expect_read_only_failure "source verification of $PREFIX" \
		"$DEPENDENCIES/prepare.sh" --verify --product all \
		--root "$PREFIX" --cache "$CACHE"
done

# The build script's manifest check reads only the manifest.
BEFORE=$(snapshot)
"$DEPENDENCIES/build.sh" --check-manifest-only \
	--prefix "$SCRATCH/absent-parent/prefix" --source-root "$SOURCES" \
	--work-root "$WORK" >/dev/null || fail "the manifest check failed"
expect_unchanged "$BEFORE" "the build manifest check"

printf '%s\n' "dependency verification read-only tests passed"
