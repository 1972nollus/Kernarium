#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Download, authenticate, and safely unpack the exact upstream source archives
# listed in manifest.tsv.  This is the only dependency step that uses network
# access; CMake configuration and compilation are deliberately offline.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MANIFEST=${UML_DEPENDENCY_MANIFEST:-"$ROOT_DIR/dependencies/manifest.tsv"}
PATCH_ROOT=${UML_DEPENDENCY_PATCH_ROOT:-"$ROOT_DIR/dependencies"}
ARCHIVE_TOOL="$ROOT_DIR/dependencies/source_archive.py"
DEPENDENCY_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
CACHE_ROOT=${UML_DEPENDENCY_CACHE:-"$ROOT_DIR/.build/dependency-cache"}
VERIFY_ONLY=0
FETCH=1
VALIDATE_MANIFEST_ONLY=0

fail()
{
	printf '%s\n' "dependencies/prepare.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: dependencies/prepare.sh [options]

Options:
  --root PATH     Prepared source root (default: external/sources)
  --cache PATH    Authenticated archive cache (default: .build/dependency-cache)
  --verify        Verify prepared trees without changing them or using network
  --no-fetch      Prepare only from archives already present in the cache
  --validate-manifest-only
                  Validate manifest rows without reading archives or trees
  -h, --help      Show this help

Preparation never replaces an existing, mismatched tree. Move it aside and
retry after reviewing why its canonical hash differs.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--root)
		[ "$#" -ge 2 ] || fail "--root requires a path"
		DEPENDENCY_ROOT=$2
		shift 2
		;;
	--cache)
		[ "$#" -ge 2 ] || fail "--cache requires a path"
		CACHE_ROOT=$2
		shift 2
		;;
	--verify)
		VERIFY_ONLY=1
		shift
		;;
	--no-fetch)
		FETCH=0
		shift
		;;
	--validate-manifest-only)
		VALIDATE_MANIFEST_ONLY=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		fail "unknown option: $1"
		;;
	esac
done

command -v python3 >/dev/null 2>&1 || fail "python3 build tool is required"
command -v shasum >/dev/null 2>&1 || fail "shasum build tool is required"
[ -x "$ARCHIVE_TOOL" ] || fail "missing archive verifier: $ARCHIVE_TOOL"
[ -r "$MANIFEST" ] || fail "missing dependency manifest: $MANIFEST"

if ! awk -F '\t' '
	/^#/ || /^[[:space:]]*$/ { next }
	$1 == "name" {
		if (header || data || NF != 10 ||
		    $2 != "version" || $3 != "kind" ||
		    $4 != "archive_url" || $5 != "archive_file" ||
		    $6 != "archive_sha256" || $7 != "archive_root" ||
		    $8 != "prepared_tree_sha256" || $9 != "patch" ||
		    $10 != "patch_sha256") {
			printf "invalid manifest header at row %d\n", NR > "/dev/stderr"
			exit 1
		}
		header = 1
		next
	}
	!header {
		printf "manifest data precedes its header at row %d\n", NR > "/dev/stderr"
		exit 1
	}
	NF != 10 {
		printf "manifest row %d has %d fields, expected 10\n", NR, NF > "/dev/stderr"
		exit 1
	}
	seen[$1]++ {
		printf "duplicate dependency %s at row %d\n", $1, NR > "/dev/stderr"
		exit 1
	}
	{ data = 1 }
	END {
		if (!header || !data) {
			print "manifest must contain one header and at least one dependency" > "/dev/stderr"
			exit 1
		}
	}
' "$MANIFEST"; then
	fail "dependency manifest structure is invalid"
fi

sha256_file()
{
	shasum -a 256 "$1" | awk '{print $1}'
}

validate_row()
{
	case "$name" in
	*[!A-Za-z0-9._+-]*|'') fail "unsafe dependency name: $name" ;;
	esac
	case "$kind" in
	release) ;;
	*) fail "$name has unsupported source kind: $kind" ;;
	esac
	[ -n "$version" ] || fail "$name has an empty version"
	case "$archive_url" in
	https://*) ;;
	*) fail "$name archive URL must use HTTPS: $archive_url" ;;
	esac
	case "$archive_file" in
	*/*|*..*|'') fail "$name has unsafe archive filename: $archive_file" ;;
	esac
	case "$archive_root" in
	*/*|*\\*|..|'') fail "$name has unsafe archive root: $archive_root" ;;
	esac
	case "$archive_sha256:$prepared_tree_sha256" in
	*[!0-9a-f:]*|*PENDING*) fail "$name has an invalid or pending source hash" ;;
	esac
	[ "${#archive_sha256}" -eq 64 ] || fail "$name archive hash is not SHA-256"
	[ "${#prepared_tree_sha256}" -eq 64 ] ||
		fail "$name prepared-tree hash is not SHA-256"
	if [ "$patch" = '-' ]; then
		[ "$patch_sha256" = '-' ] ||
			fail "$name has a patch hash but no patch"
	else
		case "$patch" in
		/*|*../*|*/..|..) fail "$name has unsafe patch path: $patch" ;;
		esac
		case "$patch_sha256" in
		*[!0-9a-f]*|'') fail "$name patch hash is not SHA-256" ;;
		esac
		[ "${#patch_sha256}" -eq 64 ] ||
			fail "$name patch hash is not SHA-256"
		patch_path="$PATCH_ROOT/$patch"
		[ -f "$patch_path" ] || fail "$name patch is missing: $patch_path"
		actual_patch_sha256=$(sha256_file "$patch_path")
		[ "$actual_patch_sha256" = "$patch_sha256" ] ||
			fail "$name patch hash changed: $patch_path"
	fi
}

validate_patch_inventory()
{
	manifest_patches=$(awk -F '\t' '
		/^#/ || /^[[:space:]]*$/ || $1 == "name" || $9 == "-" { next }
		{ print $9 }
	' "$MANIFEST" | LC_ALL=C sort -u)
	directory_patches=$(
		cd "$PATCH_ROOT"
		find . -type f -name '*.patch' -print |
			sed 's|^\./||' | LC_ALL=C sort
	)
	[ "$directory_patches" = "$manifest_patches" ] ||
		fail "dependency patch inventory differs from manifest"
}

if [ "$VALIDATE_MANIFEST_ONLY" -eq 1 ]; then
	tab=$(printf '\t')
	while IFS="$tab" read -r name version kind archive_url archive_file \
		archive_sha256 archive_root prepared_tree_sha256 patch patch_sha256; do
		case "$name" in
		\#*|name|'') continue ;;
		esac
		validate_row
	done < "$MANIFEST"
	validate_patch_inventory
	printf '%s\n' "validated dependency manifest: $MANIFEST"
	exit 0
fi

validate_patch_inventory

if [ "$VERIFY_ONLY" -eq 0 ]; then
	mkdir -p -- "$DEPENDENCY_ROOT" "$CACHE_ROOT"
else
	[ -d "$DEPENDENCY_ROOT" ] ||
		fail "dependency source root does not exist: $DEPENDENCY_ROOT"
fi
DEPENDENCY_ROOT=$(CDPATH= cd -- "$DEPENDENCY_ROOT" && pwd -P)
if [ -d "$CACHE_ROOT" ]; then
	CACHE_ROOT=$(CDPATH= cd -- "$CACHE_ROOT" && pwd -P)
fi

STAGING_ROOT=
cleanup()
{
	if [ -n "$STAGING_ROOT" ] && [ -d "$STAGING_ROOT" ]; then
		rm -rf -- "$STAGING_ROOT"
	fi
}
trap cleanup EXIT HUP INT TERM

verify_tree()
{
	verified_directory=$1
	[ -d "$verified_directory" ] ||
		fail "$name source tree is missing: $verified_directory"
	actual_tree_sha256=$(python3 "$ARCHIVE_TOOL" tree-hash \
		--root "$verified_directory")
	[ "$actual_tree_sha256" = "$prepared_tree_sha256" ] ||
		fail "$name source tree hash is $actual_tree_sha256, expected $prepared_tree_sha256"
}

obtain_archive()
{
	archive_path="$CACHE_ROOT/$archive_file"
	if [ ! -f "$archive_path" ]; then
		[ "$FETCH" -eq 1 ] || fail "$name archive is absent from offline cache: $archive_path"
		command -v curl >/dev/null 2>&1 || fail "curl build tool is required to fetch archives"
		temporary_archive=$(mktemp "$CACHE_ROOT/.${archive_file}.download.XXXXXX")
		if ! curl --fail --location --proto '=https' --tlsv1.2 \
			--retry 3 --output "$temporary_archive" "$archive_url"; then
			rm -f -- "$temporary_archive"
			fail "download failed for $name"
		fi
		actual_archive_sha256=$(sha256_file "$temporary_archive")
		[ "$actual_archive_sha256" = "$archive_sha256" ] || {
			rm -f -- "$temporary_archive"
			fail "$name downloaded archive hash is $actual_archive_sha256, expected $archive_sha256"
		}
		mv -- "$temporary_archive" "$archive_path"
	fi
	actual_archive_sha256=$(sha256_file "$archive_path")
	[ "$actual_archive_sha256" = "$archive_sha256" ] ||
		fail "$name cached archive hash is $actual_archive_sha256, expected $archive_sha256"
}

publish_exclusively()
{
	source=$1
	destination=$2
	python3 - "$source" "$destination" <<'PY'
import ctypes
import os
import sys


source = os.fsencode(sys.argv[1])
destination = os.fsencode(sys.argv[2])
libc = ctypes.CDLL(None, use_errno=True)
rename_exclusive = libc.renamex_np
rename_exclusive.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
rename_exclusive.restype = ctypes.c_int
if rename_exclusive(source, destination, 0x00000004) != 0:
    raise SystemExit(1)
PY
}

prepare_tree()
{
	directory="$DEPENDENCY_ROOT/$name"
	if [ -e "$directory" ] || [ -L "$directory" ]; then
		verify_tree "$directory"
		return
	fi

	obtain_archive
	if [ -z "$STAGING_ROOT" ]; then
		STAGING_ROOT=$(mktemp -d "$DEPENDENCY_ROOT/.prepare.XXXXXX")
	fi
	staged_tree="$STAGING_ROOT/$name"
	python3 "$ARCHIVE_TOOL" extract \
		--archive "$CACHE_ROOT/$archive_file" \
		--expected-root "$archive_root" \
		--destination "$staged_tree"
	if [ "$patch" != '-' ]; then
		command -v patch >/dev/null 2>&1 || fail "patch build tool is required"
		patch --batch --forward -p1 -d "$staged_tree" < "$patch_path" >/dev/null
	fi
	verify_tree "$staged_tree"
	if ! publish_exclusively "$staged_tree" "$directory"; then
		if [ -e "$directory" ] || [ -L "$directory" ]; then
			verify_tree "$directory"
		else
			fail "$name source tree could not be published atomically"
		fi
	fi
	verify_tree "$directory"
}

tab=$(printf '\t')
while IFS="$tab" read -r name version kind archive_url archive_file \
	archive_sha256 archive_root prepared_tree_sha256 patch patch_sha256; do
	case "$name" in
	\#*|name|'') continue ;;
	esac
	validate_row
	if [ "$VERIFY_ONLY" -eq 1 ]; then
		verify_tree "$DEPENDENCY_ROOT/$name"
	else
		prepare_tree
	fi
done < "$MANIFEST"

printf '%s\n' "verified pinned dependency sources below $DEPENDENCY_ROOT"
