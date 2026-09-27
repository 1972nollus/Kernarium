#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

PREFIX=
MANIFEST=
DEPLOYMENT_TARGET=
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREPARE_SCRIPT="$SCRIPT_DIR/prepare.sh"

fail()
{
	printf '%s\n' "dependencies/verify-prefix.sh: $*" >&2
	exit 1
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--prefix)
		[ "$#" -ge 2 ] || fail "--prefix requires a path"
		PREFIX=$2
		shift 2
		;;
	--manifest)
		[ "$#" -ge 2 ] || fail "--manifest requires a path"
		MANIFEST=$2
		shift 2
		;;
	--deployment-target)
		[ "$#" -ge 2 ] || fail "--deployment-target requires a value"
		DEPLOYMENT_TARGET=$2
		shift 2
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$PREFIX" ] && [ -n "$MANIFEST" ] && [ -n "$DEPLOYMENT_TARGET" ] ||
	fail "--prefix, --manifest, and --deployment-target are required"
[ -d "$PREFIX" ] || fail "prefix is missing: $PREFIX"
[ ! -L "$PREFIX" ] || fail "prefix must not be a symbolic link: $PREFIX"
[ -f "$MANIFEST" ] || fail "manifest is missing: $MANIFEST"
command -v xcrun >/dev/null 2>&1 || fail "xcrun build tool is required"
[ -x "$PREPARE_SCRIPT" ] || fail "source preparation policy is missing: $PREPARE_SCRIPT"

if ! manifest_validation=$(
	UML_DEPENDENCY_MANIFEST="$MANIFEST" \
	UML_DEPENDENCY_PATCH_ROOT="$SCRIPT_DIR" \
		"$PREPARE_SCRIPT" --validate-manifest-only 2>&1
); then
	printf '%s\n' "$manifest_validation" >&2
	fail "dependency source policy validation failed"
fi

XCRUN_AR=$(xcrun --find ar)
[ -x "$XCRUN_AR" ] || fail "xcrun returned an invalid ar build tool"
XCRUN_OTOOL=$(xcrun --find otool)
[ -x "$XCRUN_OTOOL" ] || fail "xcrun returned an invalid otool build tool"
XCRUN_LIPO=$(xcrun --find lipo)
[ -x "$XCRUN_LIPO" ] || fail "xcrun returned an invalid lipo build tool"

RECEIPT="$PREFIX/share/uml-dependencies/receipt.tsv"
[ -f "$RECEIPT" ] || fail "build receipt is missing: $RECEIPT"
[ ! -L "$RECEIPT" ] || fail "build receipt must not be a symbolic link"

if ! awk -F '\t' '
	NF != 2 || $1 == "" || $2 == "" {
		printf "malformed receipt row %d\n", NR > "/dev/stderr"
		exit 1
	}
	seen[$1]++ {
		printf "duplicate receipt field %s\n", $1 > "/dev/stderr"
		exit 1
	}
' "$RECEIPT"; then
	fail "build receipt is malformed"
fi

# The rows were validated above as unique, nonempty key<TAB>value pairs.
tab=$(printf '\t')
receipt_value()
{
	while IFS="$tab" read -r receipt_key receipt_field ||
		[ -n "$receipt_key" ]; do
		[ "$receipt_key" = "$1" ] || continue
		printf '%s\n' "$receipt_field"
		return 0
	done <"$RECEIPT"
	fail "receipt has no $1 field"
}

# The prefix is current while the macos rows it was built from are unchanged.
# The products column only records which prefixes share a row.
product_rows()
{
	awk -F '\t' -v OFS='\t' '
		/^#/ || /^[[:space:]]*$/ { next }
		$1 == "name" || index("," $3 ",", ",macos,") { $3 = ""; print }
	' "$1"
}
INSTALLED_MANIFEST="$PREFIX/share/uml-dependencies/manifest.tsv"
[ -f "$INSTALLED_MANIFEST" ] && [ ! -L "$INSTALLED_MANIFEST" ] ||
	fail "prefix records no dependency manifest; rebuild it with dependencies/build.sh"
[ "$(product_rows "$INSTALLED_MANIFEST")" = "$(product_rows "$MANIFEST")" ] ||
	fail "prefix was built from different macos dependency rows; rebuild it"
[ "$(receipt_value schema)" = 2 ] || fail "unsupported receipt schema"
[ "$(receipt_value platform)" = macos ] || fail "prefix is not a macOS build"
[ "$(receipt_value architecture)" = arm64 ] || fail "prefix is not arm64"
[ "$(receipt_value linkage)" = static ] || fail "prefix is not static"
[ "$(receipt_value deployment_target)" = "$DEPLOYMENT_TARGET" ] ||
	fail "prefix deployment target does not match $DEPLOYMENT_TARGET"
receipt_value sdk_version >/dev/null
receipt_value built_at_utc >/dev/null
receipt_value compiler >/dev/null

prefix_symlink=$(find "$PREFIX" -type l -print -quit)
[ -z "$prefix_symlink" ] ||
	fail "symbolic links are forbidden in the private prefix: $prefix_symlink"

required_files='
include/zlib.h
include/zconf.h
include/pcre2.h
include/ffi.h
include/iconv.h
include/libintl.h
include/glib-2.0/glib.h
lib/glib-2.0/include/glibconfig.h
include/slirp/libslirp.h
include/rfb/rfb.h
include/rfb/rfbclient.h
include/SoftFloat-3e/softfloat.h
include/SoftFloat-3e/softfloat_types.h
include/cephes_128bit.h
lib/libz.a
lib/libpcre2-8.a
lib/libffi.a
lib/libcharset.a
lib/libiconv.a
lib/libintl.a
lib/libglib-2.0.a
lib/libslirp.a
lib/libvncserver.a
lib/libvncclient.a
lib/libsoftfloat_3e.a
lib/libcephes_128bit.a'
printf '%s\n' "$required_files" | while IFS= read -r relative; do
	[ -z "$relative" ] && continue
	[ -f "$PREFIX/$relative" ] || fail "required prefix file is missing: $relative"
done

dynamic_files=$(find "$PREFIX" \( -type f -o -type l \) -print |
	awk '/\.dylib$/ || /\.so$/ || /\.so(\.[0-9]+)+$/')
[ -z "$dynamic_files" ] || fail "dynamic libraries are forbidden in the private prefix: $dynamic_files"

# Suffix checks provide a useful diagnostic for conventional library names, but
# the static-only policy is about payload contents. Inspect every regular file
# below lib so a Mach-O dynamic library or plug-in cannot hide behind an
# unconventional filename.
find "$PREFIX/lib" -type f -print | while IFS= read -r library_payload; do
	if "$XCRUN_OTOOL" -hv "$library_payload" 2>/dev/null |
		awk '$5 == "DYLIB" || $5 == "BUNDLE" || $5 == "DYLIB_STUB" { found = 1 }
			END { exit !found }'; then
		fail "dynamic Mach-O payload is forbidden in the private prefix: $library_payload"
	fi
done

for cmake_metadata in lib/cmake share/cmake; do
	[ ! -e "$PREFIX/$cmake_metadata" ] && [ ! -L "$PREFIX/$cmake_metadata" ] ||
		fail "unused installed CMake package metadata is forbidden: $cmake_metadata"
done

find "$PREFIX/lib" -type f -name '*.a' -print |
while IFS= read -r archive; do
	archive_name=${archive##*/}
	case "$archive_name" in
	libz.a|libpcre2-8.a|libffi.a|libcharset.a|libiconv.a|libintl.a|\
	libglib-2.0.a|libslirp.a|libvncserver.a|libvncclient.a|\
	libsoftfloat_3e.a|libcephes_128bit.a|\
	libgio-2.0.a|libgirepository-2.0.a|\
	libgmodule-2.0.a|libgobject-2.0.a|libgthread-2.0.a|libpcre2-posix.a)
		;;
	*) fail "undocumented static archive in dependency prefix: $archive_name" ;;
	esac
	"$XCRUN_AR" -t "$archive" >/dev/null 2>&1 ||
		fail "$archive_name is not a static archive"
	architectures=$("$XCRUN_LIPO" -archs "$archive" 2>/dev/null || true)
	[ "$architectures" = arm64 ] ||
		fail "$archive_name has unexpected architectures: $architectures"
done

printf '%s\n' "verified source-built dependency prefix: $PREFIX"
