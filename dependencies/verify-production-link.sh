#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

BINARY=
LINK_MAP=
PREFIX=
BUILD_ROOT=

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
	--build-root) BUILD_ROOT=$2; shift 2 ;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -f "$BINARY" ] || fail "linked production executable is missing: $BINARY"
[ -f "$LINK_MAP" ] || fail "production link map is missing: $LINK_MAP"
[ -d "$PREFIX" ] || fail "dependency prefix is missing: $PREFIX"
[ -d "$BUILD_ROOT" ] || fail "build root is missing: $BUILD_ROOT"
command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
OTOOL=$(xcrun --find otool)
CLANG=$(xcrun --find clang)
SDK_ROOT=$(xcrun --sdk macosx --show-sdk-path)
TOOLCHAIN_ROOT=${CLANG%/usr/bin/clang}
[ "$TOOLCHAIN_ROOT" != "$CLANG" ] ||
	fail "could not derive the Apple toolchain root from: $CLANG"

canonical_directory()
{
	directory=$1
	[ -d "$directory" ] || fail "allowed link root is missing: $directory"
	CDPATH= cd -- "$directory" && pwd -P
}

canonical_file()
{
	candidate=$1
	case "$candidate" in
	/*) ;;
	*) candidate="$CANONICAL_BUILD_ROOT/$candidate" ;;
	esac

	# Resolve directory symlinks through pwd -P and resolve a final file symlink
	# explicitly. This prevents a build-tree symlink from laundering an ambient
	# archive into the allowlist.
	resolutions=0
	while :; do
		parent=$(dirname -- "$candidate")
		name=$(basename -- "$candidate")
		physical_parent=$(CDPATH= cd -- "$parent" 2>/dev/null && pwd -P) ||
			fail "linked object parent is unavailable: $candidate"
		candidate="$physical_parent/$name"
		[ -L "$candidate" ] || break
		link=$(readlink -- "$candidate") ||
			fail "could not resolve linked object symlink: $candidate"
		case "$link" in
		/*) candidate=$link ;;
		*) candidate="$physical_parent/$link" ;;
		esac
		resolutions=$((resolutions + 1))
		[ "$resolutions" -le 32 ] ||
			fail "linked object has an excessive symlink chain: $1"
	done
	[ -f "$candidate" ] || fail "linked object is unavailable: $candidate"
	printf '%s\n' "$candidate"
}

CANONICAL_BUILD_ROOT=$(canonical_directory "$BUILD_ROOT")
CANONICAL_PREFIX=$(canonical_directory "$PREFIX")
CANONICAL_SDK_ROOT=$(canonical_directory "$SDK_ROOT")
CANONICAL_TOOLCHAIN_ROOT=$(canonical_directory "$TOOLCHAIN_ROOT")

"$OTOOL" -L "$BINARY" | awk 'NR > 1 { print $1 }' |
while IFS= read -r dependency; do
	case "$dependency" in
	/System/Library/*|/usr/lib/*) ;;
	*) fail "production executable has a non-system dynamic dependency: $dependency" ;;
	esac
done

if ! LC_ALL=C awk '
	/^# Object files:/ {
		++headers
		objects = 1
		next
	}
	/^# / { objects = 0 }
	objects {
		object = $0
		sub(/^\[[^]]*\][[:space:]]*/, "", object)
		if (object != "" && object != "linker synthesized")
			++real_objects
	}
	END {
		if (headers != 1 || real_objects == 0) {
			printf "expected one nonempty Object files section, got headers=%d objects=%d\n",
				headers, real_objects > "/dev/stderr"
			exit 1
		}
	}
' "$LINK_MAP"; then
	fail "production link map has no unique nonempty object inventory"
fi

LC_ALL=C awk '
	/^# Object files:/ { objects = 1; next }
	/^# / { objects = 0 }
	objects {
		object = $0
		sub(/^\[[^]]*\][[:space:]]*/, "", object)
		sub(/\([^()]*\)$/, "", object)
		print object
	}
' "$LINK_MAP" | sort -u |
while IFS= read -r object; do
	[ -n "$object" ] || continue
	[ "$object" = "linker synthesized" ] && continue
	canonical_object=$(canonical_file "$object")
	case "$canonical_object" in
	"$CANONICAL_BUILD_ROOT"/*) ;;
	"$CANONICAL_PREFIX"/*)
		case "$(basename -- "$canonical_object")" in
		libz.a|libpcre2-8.a|libcharset.a|libiconv.a|libintl.a|\
		libglib-2.0.a|libslirp.a|libepoxy.a|libvncserver.a|libvirglrenderer.a)
			;;
		*) fail "production link used an unowned prefix archive: $canonical_object" ;;
		esac
		;;
	"$CANONICAL_SDK_ROOT"/*|"$CANONICAL_TOOLCHAIN_ROOT"/*) ;;
	*) fail "production link used an object outside the allowed roots: $canonical_object" ;;
	esac
done

printf '%s\n' "verified production static-link closure: $BINARY"
