#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Repository tooling commits with the Git identity its environment supplies
# and carries none of its own. Prove both halves of that contract: no tool
# names an identity, and every replay helper stops with one actionable message
# before doing any work when the environment supplies nothing.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
GIT_IDENTITY="$ROOT_DIR/scripts/git-identity.sh"
PYTHON=${UML_PYTHON:-python3}
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-git-identity.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

ADVICE='Git identity is not configured: set user.name and user.email (git config --global) or GIT_COMMITTER_NAME/GIT_COMMITTER_EMAIL'

fail()
{
	printf '%s\n' "git identity contract: $*" >&2
	exit 1
}

[ -r "$GIT_IDENTITY" ] || fail "missing Git identity helper: $GIT_IDENTITY"
[ -r "$ROOT_DIR/scripts/git_identity.py" ] ||
	fail "missing Git identity helper: $ROOT_DIR/scripts/git_identity.py"

# No first-party tool may name a committer. Generated mail series carry the
# authentic author of each replayed commit and are not tooling configuration.
named=$(git -C "$ROOT_DIR" grep -l -E 'user\.(name|email)=' -- \
	android dependencies kernel scripts tests tools \
	':!*/patches/*' ':!*/series/*' || true)
[ -z "$named" ] ||
	fail "these files still name a Git identity: $named"

# Every tool that creates commits must also stop Git from inventing one from
# the account and host names, which is what a Linux builder would otherwise do.
for relative in \
	android/aosp-17/build/apply-orangefox-patches.sh \
	android/aosp-17/build/prepare-sources.sh \
	kernel/mainline-6.18/scripts/check-next-version.sh \
	scripts/git_identity.py \
	scripts/patch_series.py \
	tests/tooling/android/test_jelly_source_contract.py; do
	tool="$ROOT_DIR/$relative"
	[ -r "$tool" ] || fail "missing identity consumer: $relative"
	grep -F 'user.useConfigOnly=true' "$tool" >/dev/null ||
		fail "$relative does not disable Git identity auto-detection"
done

# Tools and fixtures that replay, export, or rebase go through the shared
# check, so the suite fails clearly instead of passing with a tool-supplied
# committer.
for relative in \
	kernel/common/tooling/kernel_series.py \
	kernel/mainline-6.18/scripts/prepare-source.sh \
	kernel/mainline-6.18/scripts/rebase-patches.sh \
	scripts/patch_series.py \
	tests/tooling/cmake/test_patch_series.py \
	tests/tooling/kernel/test_kernel_series.py; do
	tool="$ROOT_DIR/$relative"
	[ -r "$tool" ] || fail "missing identity consumer: $relative"
	grep -F 'require_git_identity' "$tool" >/dev/null ||
		fail "$relative does not require an environment identity"
done

mkdir -p "$TEST_ROOT/empty"
printf '%s\n' '[user]' '	name = Configured Fixture' \
	'	email = configured-fixture@example.invalid' \
	> "$TEST_ROOT/gitconfig"

# Suppress every source of an identity, including the account and host names
# Git would otherwise use to invent one.
stripped()
{
	env -u GIT_AUTHOR_NAME -u GIT_AUTHOR_EMAIL \
		-u GIT_COMMITTER_NAME -u GIT_COMMITTER_EMAIL \
		HOME="$TEST_ROOT/empty" XDG_CONFIG_HOME="$TEST_ROOT/empty" \
		GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null \
		"$@"
}

expect_advice()
{
	label=$1
	shift
	if stripped "$@" > "$TEST_ROOT/out" 2> "$TEST_ROOT/err"; then
		fail "$label ran without a configured Git identity"
	fi
	grep -F "$ADVICE" "$TEST_ROOT/err" >/dev/null ||
		fail "$label did not print the shared advice: $(cat "$TEST_ROOT/err")"
}

expect_accepted()
{
	label=$1
	shift
	if ! "$@" > "$TEST_ROOT/out" 2> "$TEST_ROOT/err"; then
		fail "$label was rejected: $(cat "$TEST_ROOT/err")"
	fi
}

# The kernel replay and rebase helpers refuse before they resolve a source
# checkout, so this stays independent of the developer's local configuration.
expect_advice 'kernel source preparation' \
	"$ROOT_DIR/kernel/mainline-6.18/scripts/prepare-source.sh" --verify
expect_advice 'x86_64 kernel source preparation' \
	"$ROOT_DIR/kernel/mainline-6.18/scripts/prepare-source.sh" \
	--slice x86_64 --verify
expect_advice 'kernel series rebase' \
	"$ROOT_DIR/kernel/mainline-6.18/scripts/rebase-patches.sh"
expect_advice 'dependency preparation' \
	"$ROOT_DIR/dependencies/prepare.sh" --validate-manifest-only --no-fetch
expect_advice 'Jelly source contract fixture' \
	"$PYTHON" -B \
	"$ROOT_DIR/tests/tooling/android/test_jelly_source_contract.py" \
	--source-root "$ROOT_DIR"
expect_advice 'author-only environment' \
	env GIT_AUTHOR_NAME='Author Only' \
	GIT_AUTHOR_EMAIL='author-only@example.invalid' \
	"$ROOT_DIR/dependencies/prepare.sh" --validate-manifest-only --no-fetch
expect_advice 'shared Python identity check' \
	"$PYTHON" -B -c 'import sys
sys.dont_write_bytecode = True
sys.path.insert(0, sys.argv[1])
import git_identity
git_identity.require_git_identity("probe")' "$ROOT_DIR/scripts"

# The same command must run once Git can resolve an identity, from either
# source Git reads, so the refusals above prove the contract and not a broken
# tool.
expect_accepted 'dependency preparation with an environment identity' \
	env -u GIT_AUTHOR_NAME -u GIT_AUTHOR_EMAIL \
	HOME="$TEST_ROOT/empty" XDG_CONFIG_HOME="$TEST_ROOT/empty" \
	GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null \
	GIT_COMMITTER_NAME='Environment Fixture' \
	GIT_COMMITTER_EMAIL='environment-fixture@example.invalid' \
	"$ROOT_DIR/dependencies/prepare.sh" --validate-manifest-only --no-fetch

expect_accepted 'dependency preparation with a configured identity' \
	env -u GIT_AUTHOR_NAME -u GIT_AUTHOR_EMAIL \
	-u GIT_COMMITTER_NAME -u GIT_COMMITTER_EMAIL \
	HOME="$TEST_ROOT/empty" XDG_CONFIG_HOME="$TEST_ROOT/empty" \
	GIT_CONFIG_GLOBAL="$TEST_ROOT/gitconfig" GIT_CONFIG_SYSTEM=/dev/null \
	"$ROOT_DIR/dependencies/prepare.sh" --validate-manifest-only --no-fetch

# The helper exports what Git resolved, so a replay step that suppresses the
# global configuration still commits as the caller rather than as nobody.
resolved=$(env -u GIT_AUTHOR_NAME -u GIT_AUTHOR_EMAIL \
	-u GIT_COMMITTER_NAME -u GIT_COMMITTER_EMAIL \
	HOME="$TEST_ROOT/empty" XDG_CONFIG_HOME="$TEST_ROOT/empty" \
	GIT_CONFIG_GLOBAL="$TEST_ROOT/gitconfig" GIT_CONFIG_SYSTEM=/dev/null \
	sh -c '. "$1"
uml_require_git_identity probe
GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
	git -c user.useConfigOnly=true var GIT_COMMITTER_IDENT' \
	sh "$GIT_IDENTITY")
case "$resolved" in
'Configured Fixture <configured-fixture@example.invalid> '*) ;;
*) fail "the helper did not export the configured identity: $resolved" ;;
esac

printf '%s\n' 'Git identity environment contract passed'
