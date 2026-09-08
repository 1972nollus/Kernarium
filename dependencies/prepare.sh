#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Materialize immutable upstream Git commits and replay only the ordered mail
# patches declared in manifest.tsv. This is the sole dependency step allowed
# to access the network.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MANIFEST=${UML_DEPENDENCY_MANIFEST:-"$ROOT_DIR/dependencies/manifest.tsv"}
TOOLCHAIN_MANIFEST=${UML_DEPENDENCY_TOOLCHAIN_MANIFEST:-"$ROOT_DIR/dependencies/toolchains.tsv"}
PATCH_ROOT=${UML_DEPENDENCY_PATCH_ROOT:-"$ROOT_DIR/dependencies"}
DEPENDENCY_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
CACHE_ROOT=${UML_DEPENDENCY_CACHE:-"$ROOT_DIR/.build/dependency-cache"}
TOOLCHAIN_ROOT=${UML_DEPENDENCY_TOOLCHAIN_ROOT:-"$ROOT_DIR/.build/dependency-toolchains"}
PRODUCT=${UML_DEPENDENCY_PRODUCT:-macos}
TOOLCHAIN=
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
  --product NAME  macos (default), ios-network, ios-storage, ios-compression,
                  ios-x86, wine-arm64ec-rootfs, apple-gpu,
                  apple-gpu-vulkan-experiment, x86-conformance, or all
  --root PATH     Prepared source root (default: external/sources)
  --cache PATH    Partial Git cache (default: .build/dependency-cache)
  --toolchain NAME
                  Prepare one immutable binary toolchain archive instead of
                  Git sources
  --toolchain-root PATH
                  Prepared toolchain root (default: .build/dependency-toolchains)
  --verify        Verify prepared repositories without changing or fetching
  --no-fetch      Prepare only from commits already present in the Git cache
  --validate-manifest-only
                  Validate every source and patch-series identity only
  -h, --help      Show this help

Preparation never replaces an existing, mismatched repository. Move it aside
after reviewing the mismatch, then rerun preparation.
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
	--toolchain)
		[ "$#" -ge 2 ] || fail "--toolchain requires a name"
		TOOLCHAIN=$2
		shift 2
		;;
	--toolchain-root)
		[ "$#" -ge 2 ] || fail "--toolchain-root requires a path"
		TOOLCHAIN_ROOT=$2
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
	--validate-manifest-only)
		VALIDATE_MANIFEST_ONLY=1
		shift
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
case "$TOOLCHAIN" in
*[!A-Za-z0-9._+-]*) fail "unsafe dependency toolchain: $TOOLCHAIN" ;;
esac

sha256_file()
{
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | awk '{print $1}'
	else
		sha256sum "$1" | awk '{print $1}'
	fi
}

prepare_toolchain()
{
	command -v python3 >/dev/null 2>&1 || fail "python3 build tool is required"
	if ! command -v shasum >/dev/null 2>&1 &&
		! command -v sha256sum >/dev/null 2>&1; then
		fail "shasum or sha256sum build tool is required"
	fi
	[ -r "$TOOLCHAIN_MANIFEST" ] ||
		fail "missing dependency toolchain manifest: $TOOLCHAIN_MANIFEST"
	awk -F '\t' '
		/^#/ || /^[[:space:]]*$/ { next }
		$1 == "name" {
			if (header || data || NF != 6 || $2 != "version" ||
			    $3 != "host" || $4 != "archive_url" ||
			    $5 != "sha256" || $6 != "archive_root") exit 1
			header = 1
			next
		}
		!header || NF != 6 || seen[$1]++ { exit 1 }
		{ data = 1 }
		END { if (!header || !data) exit 1 }
	' "$TOOLCHAIN_MANIFEST" || fail "dependency toolchain manifest is invalid"
	matched=0
	tab=$(printf '\t')
	while IFS="$tab" read -r toolchain_name toolchain_version toolchain_host \
		archive_url archive_sha256 archive_root; do
		case "$toolchain_name" in
		\#*|name|'') continue ;;
		esac
		case "$toolchain_name:$toolchain_version:$toolchain_host" in
		*[!A-Za-z0-9._+:-]*) fail "unsafe toolchain identity" ;;
		esac
		case "$archive_url" in
		https://*.tar.xz) ;;
		*) fail "$toolchain_name has an unsupported archive URL" ;;
		esac
		case "$archive_sha256" in
		*[!0-9a-f]*|'') fail "$toolchain_name has an invalid archive hash" ;;
		esac
		[ "${#archive_sha256}" -eq 64 ] ||
			fail "$toolchain_name archive hash must be SHA-256"
		case "$archive_root" in
		''|/*|*\\*|..|../*|*/../*|*/..|*//* )
			fail "$toolchain_name has an unsafe archive root" ;;
		esac
		[ "$toolchain_name" = "$TOOLCHAIN" ] || continue
		matched=1
		[ "$VALIDATE_MANIFEST_ONLY" -eq 0 ] || continue
		destination="$TOOLCHAIN_ROOT/$toolchain_name-$toolchain_version-$toolchain_host"
		expected_receipt=$(printf '%s\n' \
			"name=$toolchain_name" \
			"version=$toolchain_version" \
			"host=$toolchain_host" \
			"archive_url=$archive_url" \
			"sha256=$archive_sha256" \
			"archive_root=$archive_root")
		if [ -e "$destination" ] || [ -L "$destination" ]; then
			[ -d "$destination" ] || fail "$toolchain_name destination is not a directory"
			actual_receipt=$(cat "$destination/.uml-toolchain-receipt" 2>/dev/null || true)
			[ "$actual_receipt" = "$expected_receipt" ] ||
				fail "$toolchain_name prepared receipt differs"
			for binary in clang llvm-dlltool lld-link; do
				[ -x "$destination/bin/$binary" ] ||
					fail "$toolchain_name is missing executable bin/$binary"
			done
			printf '%s\n' "verified dependency toolchain: $destination"
			continue
		fi
		[ "$VERIFY_ONLY" -eq 0 ] || fail "$toolchain_name is not prepared: $destination"
		mkdir -p -- "$CACHE_ROOT/archives" "$TOOLCHAIN_ROOT"
		archive="$CACHE_ROOT/archives/$toolchain_name-$toolchain_version-$archive_sha256.tar.xz"
		if [ ! -f "$archive" ]; then
			[ "$FETCH" -eq 1 ] || fail "$toolchain_name archive is absent: $archive"
			command -v curl >/dev/null 2>&1 || fail "curl build tool is required"
			temporary_archive=$(mktemp "$CACHE_ROOT/archives/.${toolchain_name}.XXXXXX")
			if ! curl --fail --location --retry 3 --output "$temporary_archive" "$archive_url"; then
				rm -f -- "$temporary_archive"
				fail "$toolchain_name archive download failed"
			fi
			[ "$(sha256_file "$temporary_archive")" = "$archive_sha256" ] || {
				rm -f -- "$temporary_archive"
				fail "$toolchain_name downloaded archive hash differs"
			}
			if ! mv -n -- "$temporary_archive" "$archive" 2>/dev/null; then
				rm -f -- "$temporary_archive"
				[ -f "$archive" ] || fail "$toolchain_name archive could not be published"
			fi
		fi
		[ "$(sha256_file "$archive")" = "$archive_sha256" ] ||
			fail "$toolchain_name cached archive hash differs"
		staging=$(mktemp -d "$TOOLCHAIN_ROOT/.${toolchain_name}.XXXXXX")
		python3 - "$archive" "$staging" <<'PY'
import pathlib
import sys
import tarfile

archive = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2])
with tarfile.open(archive, "r:xz") as source:
    for member in source.getmembers():
        path = pathlib.PurePosixPath(member.name)
        if path.is_absolute() or ".." in path.parts:
            raise SystemExit(f"unsafe archive member: {member.name}")
    source.extractall(destination, filter="data")
PY
		[ -d "$staging/$archive_root" ] || {
			rm -rf -- "$staging"
			fail "$toolchain_name archive root differs"
		}
		printf '%s\n' "$expected_receipt" \
			> "$staging/$archive_root/.uml-toolchain-receipt"
		for binary in clang llvm-dlltool lld-link; do
			[ -x "$staging/$archive_root/bin/$binary" ] || {
				rm -rf -- "$staging"
				fail "$toolchain_name archive is missing executable bin/$binary"
			}
		done
		if ! mv -n -- "$staging/$archive_root" "$destination" 2>/dev/null; then
			rm -rf -- "$staging"
			[ -d "$destination" ] || fail "$toolchain_name could not be published"
		else
			rm -rf -- "$staging"
		fi
		actual_receipt=$(cat "$destination/.uml-toolchain-receipt" 2>/dev/null || true)
		[ "$actual_receipt" = "$expected_receipt" ] ||
			fail "$toolchain_name published receipt differs"
		printf '%s\n' "verified dependency toolchain: $destination"
	done < "$TOOLCHAIN_MANIFEST"
	[ "$matched" -eq 1 ] || fail "toolchain manifest has no entry named $TOOLCHAIN"
	[ "$VALIDATE_MANIFEST_ONLY" -eq 0 ] ||
		printf '%s\n' "validated dependency toolchain manifest: $TOOLCHAIN_MANIFEST"
}

if [ -n "$TOOLCHAIN" ]; then
	prepare_toolchain
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

mail_subject()
{
	git mailinfo /dev/null /dev/null < "$1" |
		sed -n 's/^Subject: //p'
}

validate_series()
{
	[ "$patch_series" != '-' ] || return 0
	resolve_series
	[ -f "$series_path" ] || fail "$name patch series is missing: $series_path"
	awk -F '\t' '
		/^#/ || /^[[:space:]]*$/ { next }
		$1 == "order" {
			if (header || data || NF != 5 || $2 != "patch" ||
			    $3 != "stable_patch_id" || $4 != "sha256" ||
			    $5 != "subject") exit 1
			header = 1
			next
		}
		!header || NF != 5 { exit 1 }
		{ data = 1 }
		END { if (!header || !data) exit 1 }
	' "$series_path" || fail "$name patch-series structure is invalid"
	series_directory=$(dirname -- "$series_path")
	expected_order=1
	series_count=0
	while IFS="$tab" read -r order patch_file patch_id digest subject; do
		case "$order" in
		\#*|order|'') continue ;;
		esac
		[ "$order" = "$expected_order" ] ||
			fail "$name patch-series order $order is not $expected_order"
		case "$patch_file" in
		''|*/*|*..*|*[!A-Za-z0-9._-]*)
			fail "$name has an unsafe patch filename: $patch_file"
			;;
		esac
		case "$patch_id:$digest" in
		*[!0-9a-f:]*) fail "$name has an invalid patch identity" ;;
		esac
		[ "${#patch_id}" -eq 40 ] && [ "${#digest}" -eq 64 ] ||
			fail "$name has an invalid patch identity"
		[ -n "$subject" ] || fail "$name patch subject is empty"
		patch_path="$series_directory/$patch_file"
		verify_metadata_path "$patch_path"
		[ -f "$patch_path" ] || fail "$name patch is missing: $patch_path"
		[ "$(sha256_file "$patch_path")" = "$digest" ] ||
			fail "$name patch hash changed: $patch_file"
		grep -Eq '^From [0-9a-f]{40} Mon Sep 17 00:00:00 2001$' \
			"$patch_path" ||
			fail "$name patch was not emitted by git format-patch: $patch_file"
		actual_patch_id=$(git patch-id --stable < "$patch_path" |
			awk 'NF { count++; id = $1 } END { if (count == 1) print id }')
		[ "$actual_patch_id" = "$patch_id" ] ||
			fail "$name stable patch ID changed: $patch_file"
		actual_subject=$(mail_subject "$patch_path")
		[ "$actual_subject" = "$subject" ] ||
			fail "$name patch subject changed: $patch_file"
		series_count=$((series_count + 1))
		expected_order=$((expected_order + 1))
	done < "$series_path"
	[ "$series_count" -gt 0 ] || fail "$name patch series is empty"
	if [ "$patch_series" = rootfs-vulkan:patches/venus-protocol/series.tsv ]; then
		declared_patches=$(awk -F '\t' '$1 ~ /^[0-9]+$/ { print $2 }' \
			"$series_path" | LC_ALL=C sort)
		owned_patches=$(find "$series_directory" -type f -name '*.patch' \
			-exec basename {} \; | LC_ALL=C sort)
		[ "$declared_patches" = "$owned_patches" ] ||
			fail "venus-protocol patch inventory differs from its owning series"
	fi
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
				/^#/ || /^[[:space:]]*$/ || $1 == "order" { next }
				{ print prefix $2 }
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

selected_product()
{
	[ "$PRODUCT" = all ] && return 0
	case ",$products," in
	*",$PRODUCT,"*) return 0 ;;
	esac
	return 1
}

series_values()
{
	SERIES_IDS=
	SERIES_SUBJECTS=
	SERIES_COUNT=0
	[ "$patch_series" != '-' ] || return 0
	resolve_series
	while IFS="$tab" read -r order patch_file patch_id digest subject; do
		case "$order" in
		\#*|order|'') continue ;;
		esac
		SERIES_IDS="${SERIES_IDS}${patch_id}
"
		SERIES_SUBJECTS="${SERIES_SUBJECTS}${subject}
"
		SERIES_COUNT=$((SERIES_COUNT + 1))
	done < "$series_path"
}

verify_repository()
{
	repository_directory=$1
	[ -d "$repository_directory/.git" ] ||
		fail "$name is not a prepared Git repository"
	[ "$(git -C "$repository_directory" config --get uml.source.repository 2>/dev/null || true)" = \
		"$repository" ] || fail "$name prepared repository URL changed"
	[ "$(git -C "$repository_directory" config --get uml.source.commit 2>/dev/null || true)" = \
		"$commit" ] || fail "$name prepared base commit changed"
	[ "$(git -C "$repository_directory" config --get uml.source.paths 2>/dev/null || true)" = \
		"$source_paths" ] || fail "$name prepared sparse paths changed"
	[ "$(git -C "$repository_directory" config --get uml.source.patchSeries 2>/dev/null || true)" = \
		"$patch_series" ] || fail "$name prepared patch series changed"
	[ -z "$(git -C "$repository_directory" remote)" ] ||
		fail "$name prepared repository retains a network remote"
	[ -z "$(git -C "$repository_directory" status --porcelain --untracked-files=all)" ] ||
		fail "$name prepared repository is dirty"
	actual_base=$(git -C "$repository_directory" rev-parse "$commit^{commit}" 2>/dev/null || true)
	[ "$actual_base" = "$commit" ] || fail "$name base commit is unavailable"
	series_values
	if [ "$SERIES_COUNT" -eq 0 ]; then
		[ "$(git -C "$repository_directory" rev-parse HEAD)" = "$commit" ] ||
			fail "$name unpatched source is not at its pinned commit"
		return
	fi
	git -C "$repository_directory" merge-base --is-ancestor "$commit" HEAD ||
		fail "$name prepared history is not based on its pinned commit"
	commits=$(git -C "$repository_directory" rev-list --reverse "$commit..HEAD")
	commit_count=$(printf '%s\n' "$commits" |
		awk 'NF { count++ } END { print count + 0 }')
	[ "$commit_count" -eq "$SERIES_COUNT" ] ||
		fail "$name prepared history has $commit_count patches, expected $SERIES_COUNT"
	[ -z "$(git -C "$repository_directory" rev-list --merges "$commit..HEAD")" ] ||
		fail "$name prepared history contains a merge"
	actual_subjects=$(git -C "$repository_directory" log --reverse --format='%s' "$commit..HEAD")
	[ "$actual_subjects" = "$(printf '%s' "$SERIES_SUBJECTS")" ] ||
		fail "$name prepared subjects differ from its series"
	actual_ids=$(printf '%s\n' "$commits" |
		while IFS= read -r prepared_commit; do
			[ -n "$prepared_commit" ] || continue
			git -C "$repository_directory" show --binary --pretty=format: "$prepared_commit" |
				git patch-id --stable | awk '{print $1}'
		done)
	[ "$actual_ids" = "$(printf '%s' "$SERIES_IDS")" ] ||
		fail "$name prepared patch IDs differ from its series"
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
		# object independently. A large sparse tree such as ANGLE can therefore
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
	if [ -e "$directory" ] || [ -L "$directory" ]; then
		verify_repository "$directory"
		return
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
	if [ "$patch_series" != '-' ]; then
		resolve_series
		series_directory=$(dirname -- "$series_path")
		while IFS="$tab" read -r order patch_file patch_id digest subject; do
			case "$order" in
			\#*|order|'') continue ;;
			esac
			if ! GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
				git -C "$staged_repository" \
				-c user.name='Yi Xie' \
				-c user.email='imxieyi@users.noreply.github.com' \
				-c core.hooksPath=/dev/null am --3way --no-gpg-sign \
				"$series_directory/$patch_file" >/dev/null; then
				git -C "$staged_repository" am --abort >/dev/null 2>&1 || true
				fail "$name patch does not apply: $patch_file"
			fi
		done < "$series_path"
	fi
	git -C "$staged_repository" config uml.source.repository "$repository"
	git -C "$staged_repository" config uml.source.commit "$commit"
	git -C "$staged_repository" config uml.source.paths "$source_paths"
	git -C "$staged_repository" config uml.source.patchSeries "$patch_series"
	verify_repository "$staged_repository"
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
