#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Materialize immutable upstream Git commits and replay only the ordered mail
# patches declared in manifest.tsv. This is the sole dependency step allowed
# to access the network.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
GIT_IDENTITY="$ROOT_DIR/scripts/git-identity.sh"
PATCH_SERIES="$ROOT_DIR/scripts/patch_series.py"
MANIFEST=${UML_DEPENDENCY_MANIFEST:-"$ROOT_DIR/dependencies/manifest.tsv"}
PATCH_ROOT=${UML_DEPENDENCY_PATCH_ROOT:-"$ROOT_DIR/dependencies"}
DEPENDENCY_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
CACHE_ROOT=${UML_DEPENDENCY_CACHE:-"$ROOT_DIR/.build/dependency-cache"}
PRODUCT=${UML_DEPENDENCY_PRODUCT:-macos}
VERIFY_ONLY=0
FETCH=1
RECREATE=0
VALIDATE_MANIFEST_ONLY=0
RESOLVE_REPOSITORY=
RESOLVE_REF=
ARCHIVE_MANIFEST=
TAG_REPOSITORY=
TAG_REF=
TAG_OBJECT=
TAG_COMMIT=
TAG_CHECKOUT=

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
  --product NAME  macos (default), ios-network, ios-storage, ios-compression,
                  apple-gpu, apple-gpu-vulkan-experiment, x86-conformance, or
                  all
  --root PATH     Prepared source root (default: external/sources)
  --cache PATH    Partial Git cache (default: .build/dependency-cache)
  --verify        Verify prepared repositories without changing or fetching
  --no-fetch      Prepare only from commits already present in the Git cache
  --recreate      Replace a clean prepared source of the same upstream whose
                  pinned commit or patch series changed
  --validate-manifest-only
                  Validate every source and patch-series identity only
  --archive-manifest PATH
                  Fetch SHA-256-pinned HTTPS archives into CACHE/archives
  --resolve-ref REPOSITORY REF
                  Print the object ID of one exact HTTPS branch/tag ref,
                  peeling annotated tags; use it to pin a source manifest
  --materialize-tag REPOSITORY REF OBJECT COMMIT CHECKOUT
                  Fetch an exact annotated tag into an existing checkout,
                  verifying both pinned identities before publishing its ref
  -h, --help      Show this help

A prepared source is identified by the tree its patch series records.
Preparation replaces an existing, mismatched repository only with --recreate,
and then only a clean one of the same upstream repository whose branches
point only at commits the Git cache already holds.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--product)
		[ "$#" -ge 2 ] || fail "--product requires a name"
		PRODUCT=$2
		shift 2
		;;
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
		FETCH=0
		shift
		;;
	--no-fetch)
		FETCH=0
		shift
		;;
	--recreate)
		RECREATE=1
		shift
		;;
	--validate-manifest-only)
		VALIDATE_MANIFEST_ONLY=1
		shift
		;;
	--archive-manifest)
		[ "$#" -ge 2 ] || fail "--archive-manifest requires a path"
		ARCHIVE_MANIFEST=$2
		shift 2
		;;
	--resolve-ref)
		[ "$#" -ge 3 ] || fail "--resolve-ref requires a repository and exact ref"
		RESOLVE_REPOSITORY=$2
		RESOLVE_REF=$3
		shift 3
		;;
	--materialize-tag)
		[ "$#" -ge 6 ] || fail "--materialize-tag requires repository, ref, object, commit, and checkout"
		TAG_REPOSITORY=$2
		TAG_REF=$3
		TAG_OBJECT=$4
		TAG_COMMIT=$5
		TAG_CHECKOUT=$6
		shift 6
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

case "$PRODUCT" in
*[!A-Za-z0-9._+-]*|'') fail "unsafe dependency product: $PRODUCT" ;;
esac

[ -f "$GIT_IDENTITY" ] || fail "missing Git identity helper: $GIT_IDENTITY"
# shellcheck disable=SC1090
. "$GIT_IDENTITY"
if [ -z "$ARCHIVE_MANIFEST" ] && [ -z "$RESOLVE_REPOSITORY" ] &&
	[ -z "$TAG_REPOSITORY" ]; then
	uml_require_git_identity 'dependencies/prepare.sh'
fi

sha256_file()
{
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | awk '{print $1}'
	else
		sha256sum "$1" | awk '{print $1}'
	fi
}

if [ -n "$ARCHIVE_MANIFEST" ]; then
	[ -z "$RESOLVE_REPOSITORY" ] && [ -z "$TAG_REPOSITORY" ] ||
		fail "archive preparation cannot be combined with ref discovery"
	python3 - "$ARCHIVE_MANIFEST" <<'PY_ARCHIVES'
import csv
import pathlib
import re
import sys
import urllib.parse

with pathlib.Path(sys.argv[1]).open() as stream:
    rows = list(csv.reader((line for line in stream if not line.startswith("#")), delimiter="\t"))
if not rows or rows[0] != ["name", "url", "sha256"] or len(rows) < 2:
    raise SystemExit("invalid dependency archive manifest header")
seen = set()
for row in rows[1:]:
    if len(row) != 3:
        raise SystemExit("invalid dependency archive manifest row")
    name, url, digest = row
    parsed = urllib.parse.urlsplit(url)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+%~-]*", name) or name == "name" or name in seen:
        raise SystemExit("unsafe or repeated dependency archive name")
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:
        raise SystemExit("dependency archives require HTTPS URLs without credentials")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise SystemExit("dependency archives require SHA-256 identities")
    seen.add(name)
PY_ARCHIVES
	if [ "$VALIDATE_MANIFEST_ONLY" -eq 1 ]; then
		printf 'validated dependency archive manifest: %s\n' "$ARCHIVE_MANIFEST"
		exit 0
	fi
	if [ "$VERIFY_ONLY" -eq 1 ]; then
		[ -d "$CACHE_ROOT/archives" ] || fail "dependency archive cache is absent"
	else
		mkdir -p -- "$CACHE_ROOT/archives"
	fi
	tab=$(printf '\t')
	while IFS="$tab" read -r archive_name archive_url archive_sha256; do
		case "$archive_name" in \#*|name|'') continue ;; esac
		archive="$CACHE_ROOT/archives/$archive_sha256-$archive_name"
		if [ ! -f "$archive" ]; then
			[ "$FETCH" -eq 1 ] && [ "$VERIFY_ONLY" -eq 0 ] ||
				fail "dependency archive is absent: $archive"
			temporary_archive=$(mktemp "$CACHE_ROOT/archives/.archive.XXXXXX")
			if ! curl --fail --location --proto '=https' --proto-redir '=https' \
				--retry 3 --connect-timeout 15 --max-time 300 \
				--output "$temporary_archive" "$archive_url"; then
				rm -f -- "$temporary_archive"
				fail "dependency archive download failed: $archive_name"
			fi
			[ "$(sha256_file "$temporary_archive")" = "$archive_sha256" ] || {
				rm -f -- "$temporary_archive"
				fail "downloaded dependency archive hash differs: $archive_name"
			}
			mv -- "$temporary_archive" "$archive"
		fi
		[ "$(sha256_file "$archive")" = "$archive_sha256" ] ||
			fail "cached dependency archive hash differs: $archive_name"
		printf 'verified dependency archive: %s\n' "$archive"
	done < "$ARCHIVE_MANIFEST"
	exit 0
fi

if [ -n "$TAG_REPOSITORY" ]; then
	[ "$FETCH" -eq 1 ] && [ "$VERIFY_ONLY" -eq 0 ] &&
		[ "$VALIDATE_MANIFEST_ONLY" -eq 0 ] && [ -z "$RESOLVE_REPOSITORY" ] ||
		fail "--materialize-tag cannot be combined with other preparation modes"
	case "$TAG_REPOSITORY" in
	https://?*) ;;
	*) fail "--materialize-tag requires an HTTPS repository" ;;
	esac
	case "$TAG_REF" in
	refs/tags/?*) ;;
	*) fail "--materialize-tag requires an exact refs/tags/ name" ;;
	esac
	git check-ref-format "$TAG_REF" >/dev/null 2>&1 || fail "invalid tag ref"
	for identity in "$TAG_OBJECT" "$TAG_COMMIT"; do
		case "$identity" in
		*[!0-9a-f]*|'') fail "tag identities must be SHA-1 object IDs" ;;
		esac
		[ "${#identity}" -eq 40 ] || fail "tag identities must be SHA-1 object IDs"
	done
	git -C "$TAG_CHECKOUT" rev-parse --git-dir >/dev/null 2>&1 ||
		fail "tag destination is not an existing Git checkout"
	existing=$(git -C "$TAG_CHECKOUT" rev-parse --verify "$TAG_REF" 2>/dev/null || true)
	[ -z "$existing" ] || [ "$existing" = "$TAG_OBJECT" ] ||
		fail "existing tag ref differs from its pinned object"
	git -C "$TAG_CHECKOUT" fetch --no-tags "$TAG_REPOSITORY" "$TAG_REF" ||
		fail "could not fetch pinned tag"
	[ "$(git -C "$TAG_CHECKOUT" rev-parse FETCH_HEAD)" = "$TAG_OBJECT" ] &&
		[ "$(git -C "$TAG_CHECKOUT" cat-file -t "$TAG_OBJECT")" = tag ] &&
		[ "$(git -C "$TAG_CHECKOUT" rev-parse "$TAG_OBJECT^{commit}")" = "$TAG_COMMIT" ] ||
		fail "fetched annotated tag differs from its pinned identities"
	git -C "$TAG_CHECKOUT" update-ref "$TAG_REF" "$TAG_OBJECT" \
		"${existing:-0000000000000000000000000000000000000000}"
	printf 'verified dependency tag: %s %s %s\n' "$TAG_REF" "$TAG_OBJECT" "$TAG_COMMIT"
	exit 0
fi

if [ -n "$RESOLVE_REPOSITORY" ]; then
	[ "$FETCH" -eq 1 ] && [ "$VERIFY_ONLY" -eq 0 ] &&
		[ "$VALIDATE_MANIFEST_ONLY" -eq 0 ] ||
		fail "--resolve-ref cannot be combined with offline or preparation modes"
	case "$RESOLVE_REPOSITORY" in
	https://?*) ;;
	*) fail "--resolve-ref requires an HTTPS repository" ;;
	esac
	case "$RESOLVE_REF" in
	refs/heads/?*|refs/tags/?*) ;;
	*) fail "--resolve-ref requires an exact refs/heads/ or refs/tags/ name" ;;
	esac
	git check-ref-format "$RESOLVE_REF" >/dev/null 2>&1 ||
		fail "--resolve-ref has an invalid ref name"
	resolved=$(git ls-remote --exit-code "$RESOLVE_REPOSITORY" \
		"$RESOLVE_REF" "$RESOLVE_REF^{}") || fail "dependency ref was not found"
	object=$(printf '%s\n' "$resolved" | awk -v ref="$RESOLVE_REF" '
		$2 == ref { if (direct) exit 1; direct = $1; next }
		$2 == ref "^{}" { if (peeled) exit 1; peeled = $1; next }
		{ exit 1 }
		END { if (!direct) exit 1; print peeled ? peeled : direct }
	') || fail "dependency ref response was ambiguous"
	case "$object" in
	*[!0-9a-f]*|'') fail "dependency ref did not resolve to a Git object ID" ;;
	esac
	[ "${#object}" -eq 40 ] || fail "dependency ref object ID must be SHA-1"
	ref_object=$(printf '%s\n' "$resolved" | awk -v ref="$RESOLVE_REF" '$2 == ref { print $1 }')
	printf 'resolved %s %s: ref-object=%s peeled-object=%s\n' \
		"$RESOLVE_REPOSITORY" "$RESOLVE_REF" "$ref_object" "$object" >&2
	printf '%s\n' "$object"
	exit 0
fi

command -v git >/dev/null 2>&1 || fail "git build tool is required"
command -v python3 >/dev/null 2>&1 || fail "python3 build tool is required"
if ! command -v shasum >/dev/null 2>&1 &&
	! command -v sha256sum >/dev/null 2>&1; then
	fail "shasum or sha256sum build tool is required"
fi
[ -r "$MANIFEST" ] || fail "missing dependency manifest: $MANIFEST"
[ -d "$PATCH_ROOT" ] || fail "missing dependency metadata root: $PATCH_ROOT"

if ! awk -F '\t' '
	/^#/ || /^[[:space:]]*$/ { next }
	$1 == "name" {
		if (header || data || NF != 7 || $2 != "version" ||
		    $3 != "products" || $4 != "repository" ||
		    $5 != "commit" || $6 != "source_paths" ||
		    $7 != "patch_series") {
			printf "invalid manifest header at row %d\n", NR > "/dev/stderr"
			exit 1
		}
		header = 1
		next
	}
	!header || NF != 7 {
		printf "malformed manifest row %d\n", NR > "/dev/stderr"
		exit 1
	}
	seen[$1]++ {
		printf "duplicate dependency %s at row %d\n", $1, NR > "/dev/stderr"
		exit 1
	}
	{ data = 1 }
	END {
		if (!header || !data) {
			print "manifest must contain one header and data" > "/dev/stderr"
			exit 1
		}
	}
' "$MANIFEST"; then
	fail "dependency manifest structure is invalid"
fi

tab=$(printf '\t')

safe_relative_path()
{
	case "$1" in
	''|/*|*\\*|..|../*|*/../*|*/..|*//* ) return 1 ;;
	esac
	return 0
}

# A shared protocol series has one source owner. The explicit namespace does
# not grant general parent-directory access to dependency metadata.
resolve_series()
{
	case "$patch_series" in
	rootfs-vulkan:*)
		series_relative=${patch_series#rootfs-vulkan:}
		[ "$name:$series_relative" = \
			'venus-protocol:patches/venus-protocol/series.tsv' ] ||
			fail "$name has an unsupported rootfs-vulkan series: $patch_series"
		metadata_outer=$(CDPATH= cd -- "$PATCH_ROOT/.." && pwd -P)
		series_owner="$metadata_outer/tools/rootfs/vulkan"
		;;
	*:*) fail "$name has an unknown patch-series owner: $patch_series" ;;
	*)
		series_relative=$patch_series
		series_owner=$(CDPATH= cd -- "$PATCH_ROOT" && pwd -P)
		;;
	esac
	safe_relative_path "$series_relative" ||
		fail "$name has an unsafe patch-series path: $patch_series"
	case "$series_relative" in
	*[!A-Za-z0-9._/-]*) fail "$name has an unsafe patch-series path: $patch_series" ;;
	esac
	series_path="$series_owner/$series_relative"
	verify_metadata_path "$series_path"
}

verify_metadata_path()
{
	python3 - "$1" <<'PY' || fail "metadata path contains a symlink: $1"
import os
import sys

path = os.path.abspath(sys.argv[1])
if os.path.realpath(path) != path:
    raise SystemExit(1)
PY
}

validate_vulkan_pair()
{
	case "$name" in
	venus-protocol|vulkan-headers) ;;
	*) return 0 ;;
	esac
	metadata_outer=$(CDPATH= cd -- "$PATCH_ROOT/.." && pwd -P)
	paired_sources="$metadata_outer/tools/rootfs/vulkan/sources.tsv"
	verify_metadata_path "$paired_sources"
	[ -f "$paired_sources" ] || fail "paired Vulkan source lock is missing"
	paired_identity=$(awk -F '\t' -v component="$name" \
		'$1 == component { print $2 "\t" $3; count++ }
		END { if (count != 1) exit 1 }' "$paired_sources") ||
		fail "$name paired Vulkan source identity is missing or duplicated"
	[ "$paired_identity" = "$(printf '%s\t%s' "$repository" "$commit")" ] ||
		fail "$name host and guest Vulkan source identities differ"
	[ "$products" = apple-gpu-vulkan-experiment ] && [ "$source_paths" = . ] ||
		fail "$name is a full-source experimental Vulkan dependency only"
	if [ "$name" = venus-protocol ]; then
		[ "$patch_series" = rootfs-vulkan:patches/venus-protocol/series.tsv ] ||
			fail "venus-protocol must use its canonical rootfs-vulkan series"
		[ "$(awk -F '\t' '$1 == "venus-protocol" { print $7 }' "$paired_sources")" = \
			patches/venus-protocol/series.tsv ] ||
			fail "venus-protocol paired series owner differs"
	else
		[ "$patch_series" = - ] || fail "Vulkan-Headers must use the unmodified SDK"
	fi
}

validate_products()
{
	old_ifs=$IFS
	IFS=,
	set -- $products
	IFS=$old_ifs
	[ "$#" -gt 0 ] || fail "$name has no build product"
	for product_name in "$@"; do
		case "$product_name" in
		*[!A-Za-z0-9._+-]*|'') fail "$name has an unsafe build product" ;;
		esac
	done
}

validate_source_paths()
{
	old_ifs=$IFS
	IFS=,
	set -- $source_paths
	IFS=$old_ifs
	[ "$#" -gt 0 ] || fail "$name has no source paths"
	for source_path in "$@"; do
		safe_relative_path "$source_path" ||
			fail "$name has an unsafe source path: $source_path"
	done
}

series_tool()
{
	python3 -B "$PATCH_SERIES" "$@"
}

# A generated series records the tree it produces on the pinned commit. That
# tree, not the patch bytes, identifies the prepared source.
series_values()
{
	SERIES_COUNT=0
	SERIES_TREE=
	[ "$patch_series" != '-' ] || return 0
	resolve_series
	series_directory=$(dirname -- "$series_path")
	SERIES_COUNT=$(awk -F '\t' '$1 == "patch" { count++ } END { print count + 0 }' \
		"$series_path")
	SERIES_TREE=$(series_tool recorded-tree --series-dir "$series_directory") ||
		fail "$name patch series records no tree"
}

validate_series()
{
	[ "$patch_series" != '-' ] || return 0
	resolve_series
	[ "$(basename -- "$series_path")" = series.tsv ] ||
		fail "$name patch series must name a series.tsv: $patch_series"
	[ -f "$series_path" ] || fail "$name patch series is missing: $series_path"
	series_tool verify --series-dir "$(dirname -- "$series_path")" ||
		fail "$name patch series is invalid"
	series_values
	[ "$SERIES_COUNT" -gt 0 ] || fail "$name patch series is empty"
}

validate_row()
{
	case "$name" in
	*[!A-Za-z0-9._+-]*|'') fail "unsafe dependency name: $name" ;;
	esac
	[ -n "$version" ] || fail "$name has an empty version"
	validate_products
	case "$repository" in
	https://*.git) ;;
	*) fail "$name repository must be an HTTPS Git URL ending in .git" ;;
	esac
	case "$commit" in
	*[!0-9a-f]*|'') fail "$name commit is invalid" ;;
	esac
	[ "${#commit}" -eq 40 ] || fail "$name commit must be a full Git OID"
	validate_source_paths
	validate_vulkan_pair
	validate_series
}

while IFS="$tab" read -r name version products repository commit source_paths \
	patch_series; do
	case "$name" in
	\#*|name|'') continue ;;
	esac
	validate_row
done < "$MANIFEST"

paired_count=$(awk -F '\t' '$1 == "vulkan-headers" || $1 == "venus-protocol" \
	{ count++ } END { print count + 0 }' "$MANIFEST")
[ "$paired_count" -eq 0 ] || [ "$paired_count" -eq 2 ] ||
	fail "the experimental Vulkan SDK and protocol inputs must be declared together"

validate_patch_inventory()
{
	manifest_series=$(awk -F '\t' '
		/^#/ || /^[[:space:]]*$/ || $1 == "name" || $7 == "-" ||
		    $7 ~ /^rootfs-vulkan:/ { next }
		{ print $7 }
	' "$MANIFEST" | LC_ALL=C sort -u)
	directory_series=$(
		cd "$PATCH_ROOT"
		find patches -type f -name series.tsv -print 2>/dev/null | LC_ALL=C sort
	)
	[ "$directory_series" = "$manifest_series" ] ||
		fail "dependency patch-series inventory differs from manifest"
	manifest_patches=$(
		for relative_series in $manifest_series; do
			series_directory=$(dirname -- "$relative_series")
			awk -F '\t' -v prefix="$series_directory/" '
				$1 == "patch" { print prefix $2 }
			' "$PATCH_ROOT/$relative_series"
		done | LC_ALL=C sort
	)
	directory_patches=$(
		cd "$PATCH_ROOT"
		find patches -type f -name '*.patch' -print | LC_ALL=C sort
	)
	[ "$directory_patches" = "$manifest_patches" ] ||
		fail "dependency patch inventory differs from ordered series"
}

validate_patch_inventory
if [ "$VALIDATE_MANIFEST_ONLY" -eq 1 ]; then
	printf '%s\n' "validated dependency manifest: $MANIFEST"
	exit 0
fi

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
	if [ -n "${STAGING_ROOT:-}" ] && [ -d "$STAGING_ROOT" ]; then
		rm -rf -- "$STAGING_ROOT"
	fi
}
trap cleanup EXIT HUP INT TERM

rename_atomically()
{
	python3 - "$1" "$2" "$3" <<'PY'
import ctypes
import os
import sys


flags = int(sys.argv[1], 0)
source = os.fsencode(sys.argv[2])
destination = os.fsencode(sys.argv[3])
libc = ctypes.CDLL(None, use_errno=True)
renamex_np = libc.renamex_np
renamex_np.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
renamex_np.restype = ctypes.c_int
if renamex_np(source, destination, flags) != 0:
    raise SystemExit(1)
PY
}

# RENAME_EXCL: never replace a destination another process published.
publish_exclusively()
{
	rename_atomically 0x00000004 "$1" "$2"
}

# RENAME_SWAP: exchange a verified replacement with the stale repository.
swap_atomically()
{
	rename_atomically 0x00000002 "$1" "$2"
}

selected_product()
{
	[ "$PRODUCT" = all ] && return 0
	case ",$products," in
	*",$PRODUCT,"*) return 0 ;;
	esac
	return 1
}

git_value()
{
	git -C "$1" config --get "$2" 2>/dev/null || true
}

# Set REPOSITORY_PROBLEM and fail the check without leaving the script, so
# preparation can decide whether --recreate may replace the repository.
check_repository()
{
	repository_directory=$1
	REPOSITORY_PROBLEM=
	[ -d "$repository_directory/.git" ] || {
		REPOSITORY_PROBLEM="$name is not a prepared Git repository"
		return 1
	}
	[ "$(git_value "$repository_directory" uml.source.repository)" = "$repository" ] || {
		REPOSITORY_PROBLEM="$name prepared repository URL changed"
		return 1
	}
	[ "$(git_value "$repository_directory" uml.source.commit)" = "$commit" ] || {
		REPOSITORY_PROBLEM="$name prepared base commit changed"
		return 1
	}
	[ "$(git_value "$repository_directory" uml.source.paths)" = "$source_paths" ] || {
		REPOSITORY_PROBLEM="$name prepared sparse paths changed"
		return 1
	}
	[ "$(git_value "$repository_directory" uml.source.patchSeries)" = "$patch_series" ] || {
		REPOSITORY_PROBLEM="$name prepared patch series changed"
		return 1
	}
	[ -z "$(git -C "$repository_directory" remote)" ] || {
		REPOSITORY_PROBLEM="$name prepared repository retains a network remote"
		return 1
	}
	[ -z "$(git -C "$repository_directory" status --porcelain --untracked-files=all)" ] || {
		REPOSITORY_PROBLEM="$name prepared repository is dirty"
		return 1
	}
	actual_base=$(git -C "$repository_directory" rev-parse "$commit^{commit}" 2>/dev/null || true)
	[ "$actual_base" = "$commit" ] || {
		REPOSITORY_PROBLEM="$name base commit is unavailable"
		return 1
	}
	series_values
	if [ "$SERIES_COUNT" -eq 0 ]; then
		[ "$(git -C "$repository_directory" rev-parse HEAD)" = "$commit" ] || {
			REPOSITORY_PROBLEM="$name unpatched source is not at its pinned commit"
			return 1
		}
		return 0
	fi
	git -C "$repository_directory" merge-base --is-ancestor "$commit" HEAD || {
		REPOSITORY_PROBLEM="$name prepared history is not based on its pinned commit"
		return 1
	}
	actual_tree=$(git -C "$repository_directory" rev-parse 'HEAD^{tree}')
	[ "$actual_tree" = "$SERIES_TREE" ] || {
		REPOSITORY_PROBLEM="$name prepared tree is $actual_tree; its series produce $SERIES_TREE"
		return 1
	}
}

verify_repository()
{
	check_repository "$1" || fail "$REPOSITORY_PROBLEM"
}

# --recreate replaces only what preparation itself made: a clean repository of
# the same upstream whose branches point only at commits the Git cache holds.
recreatable_repository()
{
	[ ! -L "$1" ] && [ -d "$1/.git" ] || return 1
	[ "$(git_value "$1" uml.source.repository)" = "$repository" ] || return 1
	[ -z "$(git -C "$1" status --porcelain --untracked-files=all 2>/dev/null)" ] ||
		return 1
	for branch_tip in $(git -C "$1" for-each-ref --format='%(objectname)' refs/heads); do
		env GIT_NO_LAZY_FETCH=1 git -C "$CACHE_ROOT/git/$name" \
			cat-file -e "$branch_tip^{commit}" 2>/dev/null || return 1
	done
}

obtain_cache()
{
	cache_parent="$CACHE_ROOT/git"
	cache_repository="$cache_parent/$name"
	if [ ! -e "$cache_repository" ] && [ ! -L "$cache_repository" ]; then
		[ "$FETCH" -eq 1 ] || fail "$name Git cache is absent: $cache_repository"
		mkdir -p -- "$cache_parent"
		temporary_cache=$(mktemp -d "$cache_parent/.${name}.XXXXXX")
		git -C "$temporary_cache" init --quiet
		git -C "$temporary_cache" remote add origin "$repository"
		if [ "$source_paths" = '.' ]; then
			git -C "$temporary_cache" fetch --no-tags --depth 1 \
				origin "$commit" >/dev/null
		else
			git -C "$temporary_cache" config remote.origin.promisor true
			git -C "$temporary_cache" config \
				remote.origin.partialclonefilter blob:none
			git -C "$temporary_cache" config uploadpack.allowFilter true
			git -C "$temporary_cache" config uploadpack.allowAnySHA1InWant true
			git -C "$temporary_cache" fetch --no-tags --depth 1 \
				--filter=blob:none origin "$commit" >/dev/null
		fi
		git -C "$temporary_cache" update-ref refs/heads/uml-pin "$commit"
		if ! publish_exclusively "$temporary_cache" "$cache_repository"; then
			rm -rf -- "$temporary_cache"
			[ -e "$cache_repository" ] || fail "$name Git cache could not be published"
		fi
	fi
	[ -d "$cache_repository/.git" ] || fail "$name Git cache is malformed"
	[ "$(git -C "$cache_repository" remote get-url origin 2>/dev/null || true)" = \
		"$repository" ] || fail "$name Git cache origin changed"
	if [ "$source_paths" = '.' ] &&
		[ "$(git -C "$cache_repository" config --bool remote.origin.promisor 2>/dev/null || true)" = \
		true ]; then
		[ "$FETCH" -eq 1 ] || fail "$name full source is incomplete in the offline cache"
		git -C "$cache_repository" fetch --refetch --no-filter --no-tags \
			--depth 1 origin "$commit" >/dev/null
		git -C "$cache_repository" config --unset-all remote.origin.promisor || true
		git -C "$cache_repository" config --unset-all \
			remote.origin.partialclonefilter || true
	fi
	if ! env GIT_NO_LAZY_FETCH=1 git -C "$cache_repository" cat-file -e \
		"$commit^{commit}" 2>/dev/null; then
		[ "$FETCH" -eq 1 ] || fail "$name commit is absent from the offline cache"
		if [ "$source_paths" = '.' ]; then
			git -C "$cache_repository" fetch --no-tags --depth 1 \
				origin "$commit" >/dev/null
		else
			git -C "$cache_repository" fetch --no-tags --depth 1 \
				--filter=blob:none origin "$commit" >/dev/null
		fi
		git -C "$cache_repository" update-ref refs/heads/uml-pin "$commit"
	fi
	[ "$(git -C "$cache_repository" rev-parse "$commit^{commit}")" = "$commit" ] ||
		fail "$name cache resolved the wrong commit"
	[ "$(git -C "$cache_repository" rev-parse refs/heads/uml-pin 2>/dev/null || true)" = \
		"$commit" ] || fail "$name Git cache does not retain its pinned ref"
	if [ "$source_paths" = '.' ]; then
		missing=$(env GIT_NO_LAZY_FETCH=1 git -C "$cache_repository" \
			rev-list --objects --missing=print "$commit" | sed -n 's/^?//p')
		[ -z "$missing" ] || fail "$name full source is incomplete in the Git cache"
		return
	fi
	[ "$(git -C "$cache_repository" config --bool uploadpack.allowFilter 2>/dev/null || true)" = \
		true ] || fail "$name Git cache does not permit filtered local clones"
	old_ifs=$IFS
	IFS=,
	set -- $source_paths
	IFS=$old_ifs
	CACHE_HAS_COMPLETE_COMMIT=0
	missing_any=$(env GIT_NO_LAZY_FETCH=1 git -C "$cache_repository" \
		rev-list --objects --missing=print "$commit" | sed -n 's/^?//p' | \
		sed -n '1p')
	[ -n "$missing_any" ] || CACHE_HAS_COMPLETE_COMMIT=1
	if [ "$FETCH" -eq 1 ] && [ "$CACHE_HAS_COMPLETE_COMMIT" -eq 0 ]; then
		# `cat-file --batch-check` asks a promisor remote for each missing
		# object independently. A large sparse tree such as Mesa can therefore
		# create thousands of lazy-fetch subprocesses and race the shallow
		# boundary. A transient sparse checkout lets unpack-trees request the
		# complete missing-object set in one bounded fetch. Direct its object
		# writes into the shared cache, while keeping HEAD, the index, sparse
		# configuration, and worktree private to this preparation process.
		if [ -z "$STAGING_ROOT" ]; then
			STAGING_ROOT=$(mktemp -d "$DEPENDENCY_ROOT/.prepare.XXXXXX")
		fi
		materialization_repository="$STAGING_ROOT/.materialize-$name"
		git -C "$STAGING_ROOT" init --quiet ".materialize-$name"
		git -C "$materialization_repository" remote add origin "$repository"
		git -C "$materialization_repository" config remote.origin.promisor true
		git -C "$materialization_repository" config \
			remote.origin.partialclonefilter blob:none
		env GIT_OBJECT_DIRECTORY="$cache_repository/.git/objects" \
			git -C "$materialization_repository" sparse-checkout init --cone
		env GIT_OBJECT_DIRECTORY="$cache_repository/.git/objects" \
			git -C "$materialization_repository" sparse-checkout set --cone "$@"
		env GIT_OBJECT_DIRECTORY="$cache_repository/.git/objects" \
			git -C "$materialization_repository" switch --detach "$commit" \
				>/dev/null
		rm -rf -- "$materialization_repository"
	fi
	missing_selected=$(
		git -C "$cache_repository" ls-tree -r "$commit" -- "$@" |
			awk '$2 == "blob" { print $3 }' |
			env GIT_NO_LAZY_FETCH=1 git -C "$cache_repository" cat-file \
				--batch-check='%(objectname) %(objecttype)' |
			awk '$2 != "blob" { print $1; exit }'
	)
	[ -z "$missing_selected" ] ||
		fail "$name sparse source is incomplete in the Git cache: $missing_selected"
	if [ "$CACHE_HAS_COMPLETE_COMMIT" -eq 0 ]; then
		missing_any=$(env GIT_NO_LAZY_FETCH=1 git -C "$cache_repository" \
			rev-list --objects --missing=print "$commit" | \
			sed -n 's/^?//p' | sed -n '1p')
		[ -n "$missing_any" ] || CACHE_HAS_COMPLETE_COMMIT=1
	fi
}

prepare_repository()
{
	directory="$DEPENDENCY_ROOT/$name"
	replacing=0
	if [ -e "$directory" ] || [ -L "$directory" ]; then
		check_repository "$directory" && return
		[ "$RECREATE" -eq 1 ] ||
			fail "$REPOSITORY_PROBLEM; review it, then rerun with --recreate"
		recreatable_repository "$directory" ||
			fail "$REPOSITORY_PROBLEM; --recreate replaces only a clean prepared source whose branches the Git cache holds"
		replacing=1
	fi
	obtain_cache
	if [ -z "$STAGING_ROOT" ]; then
		STAGING_ROOT=$(mktemp -d "$DEPENDENCY_ROOT/.prepare.XXXXXX")
	fi
	staged_repository="$STAGING_ROOT/$name"
	if [ "$source_paths" = '.' ]; then
		git clone --quiet --no-checkout \
			"file://$cache_repository" "$staged_repository"
	elif [ "$CACHE_HAS_COMPLETE_COMMIT" -eq 1 ]; then
		# A complete shallow cache can be cloned directly without spending
		# minutes repacking the same object graph through a blob filter.
		git clone --quiet --no-checkout --no-local \
			"$cache_repository" "$staged_repository"
	else
		# A partial repository cannot safely serve another filtered local
		# clone: upload-pack may try to lazily obtain an omitted blob through
		# the clone's local promisor URL and recursively invoke itself.  Copy
		# the already verified object store into an independent repository
		# instead.  The temporary promisor marker lets sparse checkout tolerate
		# unselected blobs, while GIT_NO_LAZY_FETCH below makes publication
		# strictly offline.  Removing the remote leaves the prepared source
		# self-contained for every selected path.
		git -C "$STAGING_ROOT" init --quiet "$name"
		git -C "$staged_repository" remote add origin "$repository"
		git -C "$staged_repository" config remote.origin.promisor true
		git -C "$staged_repository" config \
			remote.origin.partialclonefilter blob:none
		cp -R "$cache_repository/.git/objects/." \
			"$staged_repository/.git/objects"
		if [ -f "$cache_repository/.git/shallow" ]; then
			cp "$cache_repository/.git/shallow" \
				"$staged_repository/.git/shallow"
		fi
		git -C "$staged_repository" update-ref refs/heads/uml-pin "$commit"
	fi
	if [ "$source_paths" != '.' ]; then
		old_ifs=$IFS
		IFS=,
		set -- $source_paths
		IFS=$old_ifs
		env GIT_NO_LAZY_FETCH=1 git -C "$staged_repository" \
			sparse-checkout set --cone "$@"
	fi
	env GIT_NO_LAZY_FETCH=1 git -C "$staged_repository" \
		switch --detach "$commit" >/dev/null
	git -C "$staged_repository" remote remove origin
	# Branches a clone copied from the cache are not part of the source.
	git -C "$staged_repository" for-each-ref --format='delete %(refname)' \
		refs/heads | git -C "$staged_repository" update-ref --stdin
	if [ "$patch_series" != '-' ]; then
		series_values
		env GIT_NO_LAZY_FETCH=1 python3 -B "$PATCH_SERIES" apply \
			--repo "$staged_repository" --series-dir "$series_directory" \
			--expect-tree "$SERIES_TREE" >/dev/null ||
			fail "$name patch series does not apply"
	fi
	git -C "$staged_repository" config uml.source.repository "$repository"
	git -C "$staged_repository" config uml.source.commit "$commit"
	git -C "$staged_repository" config uml.source.paths "$source_paths"
	git -C "$staged_repository" config uml.source.patchSeries "$patch_series"
	verify_repository "$staged_repository"
	if [ "$replacing" -eq 1 ]; then
		swap_atomically "$staged_repository" "$directory" ||
			fail "$name stale source could not be replaced atomically"
		rm -rf -- "$staged_repository"
		verify_repository "$directory"
		printf '%s\n' "recreated $name in $directory"
		return
	fi
	if ! publish_exclusively "$staged_repository" "$directory"; then
		if [ -e "$directory" ] || [ -L "$directory" ]; then
			verify_repository "$directory"
		else
			fail "$name source repository could not be published atomically"
		fi
	fi
	verify_repository "$directory"
}

matched=0
while IFS="$tab" read -r name version products repository commit source_paths \
	patch_series; do
	case "$name" in
	\#*|name|'') continue ;;
	esac
	selected_product || continue
	matched=1
	if [ "$VERIFY_ONLY" -eq 1 ]; then
		verify_repository "$DEPENDENCY_ROOT/$name"
	else
		prepare_repository
	fi
done < "$MANIFEST"

[ "$matched" -eq 1 ] || fail "manifest contains no sources for product $PRODUCT"
printf '%s\n' "verified $PRODUCT dependency repositories below $DEPENDENCY_ROOT"
