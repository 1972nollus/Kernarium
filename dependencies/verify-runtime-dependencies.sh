#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

BINARY=

fail()
{
	printf '%s\n' "dependencies/verify-runtime-dependencies.sh: $*" >&2
	exit 1
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--binary)
		[ "$#" -ge 2 ] || fail "--binary requires a path"
		BINARY=$2
		shift 2
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -f "$BINARY" ] || fail "linked production executable is missing: $BINARY"
command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
OTOOL=$(xcrun --find otool)
[ -x "$OTOOL" ] || fail "xcrun returned an invalid otool"

"$OTOOL" -L "$BINARY" | awk 'NR > 1 { print $1 }' |
while IFS= read -r dependency; do
	case "$dependency" in
	/System/Library/*|/usr/lib/*) ;;
	*) fail "production executable has a non-system dynamic dependency: $dependency" ;;
	esac
done

printf '%s\n' "verified production runtime dependency closure: $BINARY"
