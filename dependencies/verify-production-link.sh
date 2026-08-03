#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

BINARY=
LINK_MAP=
PREFIX=

fail()
{
	printf '%s\n' "dependencies/verify-production-link.sh: $*" >&2
	exit 1
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--binary) BINARY=$2; shift 2 ;;
	--link-map) LINK_MAP=$2; shift 2 ;;
	--prefix) PREFIX=$2; shift 2 ;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -f "$BINARY" ] || fail "linked production executable is missing: $BINARY"
[ -f "$LINK_MAP" ] || fail "production link map is missing: $LINK_MAP"
[ -d "$PREFIX" ] || fail "dependency prefix is missing: $PREFIX"
command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
OTOOL=$(xcrun --find otool)

"$OTOOL" -L "$BINARY" | awk 'NR > 1 { print $1 }' |
while IFS= read -r dependency; do
	case "$dependency" in
	/System/Library/*|/usr/lib/*) ;;
	*) fail "production executable has a non-system dynamic dependency: $dependency" ;;
	esac
done

if LC_ALL=C awk '
	/^# Object files:/ { objects = 1; next }
	/^# / { objects = 0 }
	objects && $0 ~ "/(opt/homebrew|usr/local|opt/local)/" { found = 1 }
	END { exit !found }
' "$LINK_MAP"; then
	fail "production link map references a host package-manager prefix"
fi

LC_ALL=C awk -v prefix="$PREFIX/lib/" '
	/^# Object files:/ { objects = 1; next }
	/^# / { objects = 0 }
	objects && index($0, prefix) {
		archive = substr($0, index($0, prefix))
		sub(/[ (].*$/, "", archive)
		print archive
	}
' "$LINK_MAP" | sort -u |
while IFS= read -r archive; do
	[ -n "$archive" ] || continue
	case "$(basename "$archive")" in
	libz.a|libpcre2-8.a|libcharset.a|libiconv.a|libintl.a|\
	libglib-2.0.a|libslirp.a|libepoxy.a|libvncserver.a|libvirglrenderer.a)
		;;
	*) fail "production link used an unowned prefix archive: $archive" ;;
	esac
done

printf '%s\n' "verified production static-link closure: $BINARY"
