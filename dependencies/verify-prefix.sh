#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

PREFIX=
MANIFEST=
BUILD_SCRIPT=
DEPLOYMENT_TARGET=
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ARCHIVE_TOOL="$SCRIPT_DIR/source_archive.py"

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
	--build-script)
		[ "$#" -ge 2 ] || fail "--build-script requires a path"
		BUILD_SCRIPT=$2
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

[ -n "$PREFIX" ] && [ -n "$MANIFEST" ] && [ -n "$BUILD_SCRIPT" ] &&
	[ -n "$DEPLOYMENT_TARGET" ] ||
	fail "--prefix, --manifest, --build-script, and --deployment-target are required"
[ -d "$PREFIX" ] || fail "prefix is missing: $PREFIX"
[ -f "$MANIFEST" ] || fail "manifest is missing: $MANIFEST"
[ -f "$BUILD_SCRIPT" ] || fail "build script is missing: $BUILD_SCRIPT"
command -v shasum >/dev/null 2>&1 || fail "shasum build tool is required"
command -v xcrun >/dev/null 2>&1 || fail "xcrun build tool is required"
command -v grep >/dev/null 2>&1 || fail "grep build tool is required"
[ -x "$ARCHIVE_TOOL" ] || fail "source archive verifier is missing: $ARCHIVE_TOOL"
XCRUN_AR=$(xcrun --find ar)
[ -x "$XCRUN_AR" ] || fail "xcrun returned an invalid ar build tool"
XCRUN_OTOOL=$(xcrun --find otool)
[ -x "$XCRUN_OTOOL" ] || fail "xcrun returned an invalid otool build tool"

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

receipt_value()
{
	key=$1
	value=$(awk -F '\t' -v wanted="$key" '$1 == wanted { print $2; exit }' "$RECEIPT")
	[ -n "$value" ] || fail "receipt has no $key field"
	printf '%s\n' "$value"
}

expected_manifest=$(shasum -a 256 "$MANIFEST" | awk '{print $1}')
expected_build_policy=$(shasum -a 256 "$BUILD_SCRIPT" | awk '{print $1}')
expected_archive_tool=$(shasum -a 256 "$ARCHIVE_TOOL" | awk '{print $1}')
[ "$(receipt_value schema)" = 2 ] || fail "unsupported receipt schema"
[ "$(receipt_value manifest_sha256)" = "$expected_manifest" ] ||
	fail "prefix was built from a different dependency manifest"
[ "$(receipt_value build_policy_sha256)" = "$expected_build_policy" ] ||
	fail "prefix was built with a different dependency build policy"
[ "$(receipt_value archive_tool_sha256)" = "$expected_archive_tool" ] ||
	fail "prefix was built with a different source archive verifier"
[ "$(receipt_value platform)" = macos ] || fail "prefix is not a macOS build"
[ "$(receipt_value architecture)" = arm64 ] || fail "prefix is not arm64"
[ "$(receipt_value linkage)" = static ] || fail "prefix is not static"
[ "$(receipt_value deployment_target)" = "$DEPLOYMENT_TARGET" ] ||
	fail "prefix deployment target does not match $DEPLOYMENT_TARGET"
receipt_value sdk_version >/dev/null
receipt_value built_at_utc >/dev/null
receipt_value compiler >/dev/null

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
include/epoxy/gl.h
include/rfb/rfb.h
include/rfb/rfbclient.h
include/virgl/virglrenderer.h
include/virgl/virgl_hw.h
lib/libz.a
lib/libpcre2-8.a
lib/libffi.a
lib/libcharset.a
lib/libiconv.a
lib/libintl.a
lib/libglib-2.0.a
lib/libslirp.a
lib/libepoxy.a
lib/libvncserver.a
lib/libvncclient.a
lib/libvirglrenderer.a'
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

for staging_marker in '/.uml-dependency-image.' '/.uml-dependency-prefix.' \
	'/source-build.'; do
	if staging_reference=$(LC_ALL=C grep -r -a -F -l \
		-e "$staging_marker" -- "$PREFIX"); then
		fail "prefix embeds a former staging path in: $staging_reference"
	else
		grep_result=$?
		[ "$grep_result" -eq 1 ] || fail "could not scan prefix for staging paths"
	fi
done

find "$PREFIX/lib" \( -type f -o -type l \) -name '*.a' -print |
while IFS= read -r archive; do
	case "$(basename "$archive")" in
	libz.a|libpcre2-8.a|libffi.a|libcharset.a|libiconv.a|libintl.a|\
	libglib-2.0.a|libslirp.a|libepoxy.a|libvncserver.a|libvncclient.a|\
	libvirglrenderer.a|libgio-2.0.a|libgirepository-2.0.a|\
	libgmodule-2.0.a|libgobject-2.0.a|libgthread-2.0.a|libpcre2-posix.a)
		;;
	*) fail "undocumented static archive in dependency prefix: $(basename "$archive")" ;;
	esac
	"$XCRUN_AR" -t "$archive" >/dev/null 2>&1 ||
		fail "$(basename "$archive") is not a static archive"
	architectures=$(xcrun lipo -archs "$archive" 2>/dev/null || true)
	[ "$architectures" = arm64 ] ||
		fail "$(basename "$archive") has unexpected architectures: $architectures"
	if ! "$XCRUN_OTOOL" -l "$archive" | awk -v expected="$DEPLOYMENT_TARGET" '
		function finish_member() {
			if (member == "")
				return
			if (!build_version || platform != "1" || minos != expected) {
				printf "%s: expected LC_BUILD_VERSION platform 1 minos %s, got platform %s minos %s\n",
					member, expected, platform, minos > "/dev/stderr"
				invalid = 1
			}
		}
		/^[^[:space:]].*\):$/ {
			finish_member()
			member = $0
			build_version = 0
			platform = ""
			minos = ""
			next
		}
		$1 == "cmd" && $2 == "LC_BUILD_VERSION" {
			build_version = 1
			next
		}
		build_version && $1 == "platform" { platform = $2; next }
		build_version && $1 == "minos" { minos = $2; next }
		END {
			finish_member()
			if (member == "")
				invalid = 1
			exit invalid
		}
	'; then
		fail "$(basename "$archive") has incompatible member platform metadata"
	fi
done

expected_payload_tree=$(receipt_value payload_tree_sha256)
actual_payload_tree=$("$ARCHIVE_TOOL" tree-hash --root "$PREFIX" \
	--exclude share/uml-dependencies/receipt.tsv)
[ "$actual_payload_tree" = "$expected_payload_tree" ] ||
	fail "dependency prefix payload hash changed: $actual_payload_tree"

printf '%s\n' "verified source-built dependency prefix: $PREFIX"
