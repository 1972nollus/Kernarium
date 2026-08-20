#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Report patch-series conflicts against a caller-selected development ref.
# This opt-in audit uses a temporary Git clone and never supplies source to a
# build or rewrites a managed pinned-commit checkout.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
METADATA="$ROOT_DIR/kernel/patches/metadata.env"
SERIES_POLICY="$ROOT_DIR/kernel/verify-series-policy.py"

fail()
{
	printf '%s\n' "kernel/check-next-version.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/check-next-version.sh --work-root PATH --ref REF [--keep]

Clone the audit-only Linux origin, detach at REF, and apply the complete tracked
series with git am --3way. Report the recorded carry, drop, retest, or upstream
action for every path when a topic applies, is already contained, or conflicts.
EOF
}

WORK_ROOT=''
UPSTREAM_REF=''
KEEP=0
while [ "$#" -gt 0 ]; do
	case "$1" in
	--work-root)
		[ "$#" -ge 2 ] || fail "--work-root requires a path"
		WORK_ROOT=$2
		shift 2
		;;
	--ref)
		[ "$#" -ge 2 ] || fail "--ref requires a value"
		UPSTREAM_REF=$2
		shift 2
		;;
	--keep)
		KEEP=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$WORK_ROOT" ] || fail "--work-root is required"
[ -n "$UPSTREAM_REF" ] || fail "--ref is required"
[ -r "$METADATA" ] || fail "missing metadata: $METADATA"
[ -x "$SERIES_POLICY" ] || fail "missing series policy verifier: $SERIES_POLICY"
# shellcheck disable=SC1090
. "$METADATA"
"$SERIES_POLICY" manifest >/dev/null
"$SERIES_POLICY" policy >/dev/null

mkdir -p -- "$WORK_ROOT"
WORK_ROOT=$(CDPATH= cd -- "$WORK_ROOT" && pwd -P)
CHECK_ROOT=$(mktemp -d "$WORK_ROOT/uml-linux-next.XXXXXX") ||
	fail "cannot create temporary checkout"
cleanup()
{
	if [ "$KEEP" -eq 0 ]; then
		rm -rf -- "$CHECK_ROOT"
	fi
}
trap cleanup 0 HUP INT TERM

audit_git()
{
	GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null git \
		-c user.name='linux-um-darwin next-version audit' \
		-c user.email='noreply@linux-um-darwin.invalid' \
		-c core.hooksPath=/dev/null "$@"
}

audit_git clone --filter=blob:none --no-checkout "$UML_KERNEL_UPSTREAM_REPOSITORY" \
	"$CHECK_ROOT/linux"
audit_git -C "$CHECK_ROOT/linux" fetch --no-tags origin "$UPSTREAM_REF"
audit_git -C "$CHECK_ROOT/linux" switch --detach FETCH_HEAD
NEXT_BASE=$(audit_git -C "$CHECK_ROOT/linux" rev-parse HEAD)

if ! "$SERIES_POLICY" next-version --source "$CHECK_ROOT/linux" \
	--base "$NEXT_BASE"; then
	exit 1
fi

printf '%s\n' "complete series applies to $UPSTREAM_REF"
if [ "$KEEP" -eq 1 ]; then
	trap - 0 HUP INT TERM
	printf '%s\n' "next-version checkout retained: $CHECK_ROOT/linux"
fi
