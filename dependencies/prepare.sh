#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Materialize immutable upstream Git commits and replay only the ordered mail
# patches declared in manifest.tsv. This is the sole dependency step allowed
# to access the network.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MANIFEST=${UML_DEPENDENCY_MANIFEST:-"$ROOT_DIR/dependencies/manifest.tsv"}
PATCH_ROOT=${UML_DEPENDENCY_PATCH_ROOT:-"$ROOT_DIR/dependencies"}
DEPENDENCY_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
CACHE_ROOT=${UML_DEPENDENCY_CACHE:-"$ROOT_DIR/.build/dependency-cache"}
PRODUCT=${UML_DEPENDENCY_PRODUCT:-macos}
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
  --product NAME  macos (default), ios-network, ios-gpu, or all
  --root PATH     Prepared source root (default: external/sources)
  --cache PATH    Partial Git cache (default: .build/dependency-cache)
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
command -v git >/dev/null 2>&1 || fail "git build tool is required"
command -v python3 >/dev/null 2>&1 || fail "python3 build tool is required"
command -v shasum >/dev/null 2>&1 || fail "shasum build tool is required"
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

sha256_file()
{
	shasum -a 256 "$1" | awk '{print $1}'
}

safe_relative_path()
{
	case "$1" in
	''|/*|*\\*|..|../*|*/../*|*/..|*//* ) return 1 ;;
	esac
	return 0
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
	safe_relative_path "$patch_series" ||
		fail "$name has an unsafe patch-series path: $patch_series"
	series_path="$PATCH_ROOT/$patch_series"
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
	validate_series
}

while IFS="$tab" read -r name version products repository commit source_paths \
	patch_series; do
	case "$name" in
	\#*|name|'') continue ;;
	esac
	validate_row
done < "$MANIFEST"

validate_patch_inventory()
{
	manifest_series=$(awk -F '\t' '
		/^#/ || /^[[:space:]]*$/ || $1 == "name" || $7 == "-" { next }
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
	series_path="$PATCH_ROOT/$patch_series"
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
			git -C "$repository_directory" show --pretty=format: "$prepared_commit" |
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
	if [ "$FETCH" -eq 1 ]; then
		git -C "$cache_repository" ls-tree -r "$commit" -- "$@" |
			awk '$2 == "blob" { print $3 }' |
			git -C "$cache_repository" cat-file \
				--batch-check='%(objectname) %(objecttype)' >/dev/null
	else
		git -C "$cache_repository" ls-tree -r "$commit" -- "$@" |
			awk '$2 == "blob" { print $3 }' |
			env GIT_NO_LAZY_FETCH=1 git -C "$cache_repository" cat-file \
				--batch-check='%(objectname) %(objecttype)' >/dev/null
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
	else
		git clone --quiet --no-checkout --filter=blob:none \
			"file://$cache_repository" "$staged_repository"
	fi
	if [ "$source_paths" != '.' ]; then
		old_ifs=$IFS
		IFS=,
		set -- $source_paths
		IFS=$old_ifs
		git -C "$staged_repository" sparse-checkout set --cone "$@"
	fi
	git -C "$staged_repository" switch --detach "$commit" >/dev/null
	git -C "$staged_repository" remote remove origin
	if [ "$patch_series" != '-' ]; then
		series_path="$PATCH_ROOT/$patch_series"
		series_directory=$(dirname -- "$series_path")
		while IFS="$tab" read -r order patch_file patch_id digest subject; do
			case "$order" in
			\#*|order|'') continue ;;
			esac
			if ! GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
				git -C "$staged_repository" \
				-c user.name='linux-um-darwin dependency replay' \
				-c user.email='noreply@linux-um-darwin.invalid' \
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
