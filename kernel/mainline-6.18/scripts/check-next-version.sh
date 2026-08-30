#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Report patch-series conflicts against a caller-selected development ref.
# This opt-in audit uses a temporary Git clone and never supplies source to a
# build or rewrites a managed pinned-commit checkout.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
SERIES_POLICY="$ROOT_DIR/kernel/common/tooling/verify-series-policy.py"
SLICE_SELECTOR="$LINE_ROOT/scripts/select-slice.sh"

fail()
{
	printf '%s\n' "kernel/mainline-6.18/scripts/check-next-version.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/mainline-6.18/scripts/check-next-version.sh \
       --work-root PATH --ref REF [--slice arm64|x86_64] [--keep]

Clone the audit-only Linux origin, detach at REF, and apply the complete tracked
series with git am --3way. Report the recorded local carry, drop, rebase, and
retest action for every path when a topic applies, is already contained, or
conflicts. This command never submits or pushes a change.
EOF
}

WORK_ROOT=''
UPSTREAM_REF=''
KEEP=0
SLICE=arm64
while [ "$#" -gt 0 ]; do
	case "$1" in
	--slice)
		[ "$#" -ge 2 ] || fail "--slice requires arm64 or x86_64"
		SLICE=$2
		shift 2
		;;
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
[ -r "$SLICE_SELECTOR" ] || fail "missing slice selector: $SLICE_SELECTOR"
# shellcheck disable=SC1090
. "$SLICE_SELECTOR"
uml_kernel_select_slice "$LINE_ROOT" "$SLICE" ||
	fail "cannot select kernel slice: $SLICE"
METADATA=$UML_KERNEL_METADATA_PATH
POLICY_ROOT=$UML_KERNEL_POLICY_ROOT
[ -r "$METADATA" ] || fail "missing metadata: $METADATA"
[ -x "$SERIES_POLICY" ] || fail "missing series policy verifier: $SERIES_POLICY"
"$SERIES_POLICY" --line-root "$POLICY_ROOT" manifest >/dev/null
"$SERIES_POLICY" --line-root "$POLICY_ROOT" policy >/dev/null
if [ "$UML_KERNEL_SLICE" = x86_64 ]; then
	"$SERIES_POLICY" --line-root "$LINE_ROOT" manifest >/dev/null
	"$SERIES_POLICY" --line-root "$LINE_ROOT" policy >/dev/null
fi

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

if [ "$UML_KERNEL_SLICE" = x86_64 ]; then
	if ! "$SERIES_POLICY" --line-root "$LINE_ROOT" next-version \
		--source "$CHECK_ROOT/linux" --base "$NEXT_BASE"; then
		exit 1
	fi
	OVERLAY_BASE=$(audit_git -C "$CHECK_ROOT/linux" rev-parse HEAD)
	if ! "$SERIES_POLICY" --line-root "$POLICY_ROOT" next-version \
		--source "$CHECK_ROOT/linux" --base "$OVERLAY_BASE"; then
		exit 1
	fi
else
	if ! "$SERIES_POLICY" --line-root "$POLICY_ROOT" next-version \
		--source "$CHECK_ROOT/linux" --base "$NEXT_BASE"; then
		exit 1
	fi
fi

printf '%s\n' "$UML_KERNEL_SLICE series composition applies to $UPSTREAM_REF"
if [ "$KEEP" -eq 1 ]; then
	trap - 0 HUP INT TERM
	printf '%s\n' "next-version checkout retained: $CHECK_ROOT/linux"
fi
