#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Authenticate and materialize the pinned kernel.org release archive as a
# small local Git repository.  Git is used only to replay and audit the
# project patch series; the upstream build input is the signed release archive.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
METADATA_PATH=${UML_KERNEL_METADATA_PATH:-"$ROOT_DIR/kernel/patches/metadata.env"}
SOURCE_DIR=''
CACHE_ROOT=${UML_KERNEL_CACHE_DIR:-"$ROOT_DIR/.build/kernel-cache"}
FETCH=1
WORK_ROOT=''
DOWNLOAD_TMP=''
KEYRING=''
GPG_TEMP_ROOT=${UML_KERNEL_GPG_TEMP_ROOT:-/tmp}

fail()
{
	printf '%s\n' "kernel/materialize-release.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/materialize-release.sh --source PATH [options]

Authenticate the pinned official Linux release archive, extract it, verify
its exact packaged Git tree, and initialize the local replay repository.

Options:
  --source PATH   New source checkout to create (required)
  --cache PATH    Authenticated archive cache (default: .build/kernel-cache)
  --no-fetch      Use only archive and signature files already in the cache
  -h, --help      Show this help

The destination must not already exist. Preparation is atomic: a failed
authentication, extraction, or tree check never leaves a partial checkout.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--source)
		[ "$#" -ge 2 ] || fail "--source requires a path"
		SOURCE_DIR=$2
		shift 2
		;;
	--cache)
		[ "$#" -ge 2 ] || fail "--cache requires a path"
		CACHE_ROOT=$2
		shift 2
		;;
	--no-fetch)
		FETCH=0
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

[ -n "$SOURCE_DIR" ] || fail "--source is required"
[ -r "$METADATA_PATH" ] || fail "missing metadata: $METADATA_PATH"
# This is checked-in project metadata. Tests may explicitly substitute a
# temporary fixture through UML_KERNEL_METADATA_PATH.
# shellcheck disable=SC1090
. "$METADATA_PATH"

[ "${UML_KERNEL_SERIES_SCHEMA:-}" = 2 ] ||
	fail "unsupported kernel source/series schema"
[ -n "${UML_KERNEL_RELEASE_VERSION:-}" ] || fail "release version is missing"
[ -n "${UML_KERNEL_ARCHIVE_URL:-}" ] || fail "archive URL is missing"
[ -n "${UML_KERNEL_ARCHIVE_FILE:-}" ] || fail "archive filename is missing"
[ -n "${UML_KERNEL_ARCHIVE_SHA256:-}" ] || fail "archive hash is missing"
[ -n "${UML_KERNEL_ARCHIVE_ROOT:-}" ] || fail "archive root is missing"
[ -n "${UML_KERNEL_SIGNATURE_URL:-}" ] || fail "signature URL is missing"
[ -n "${UML_KERNEL_SIGNATURE_FILE:-}" ] || fail "signature filename is missing"
[ -n "${UML_KERNEL_SIGNATURE_SHA256:-}" ] || fail "signature hash is missing"
[ -n "${UML_KERNEL_SIGNER_FINGERPRINT:-}" ] || fail "signer fingerprint is missing"
[ -n "${UML_KERNEL_TRUSTED_KEY:-}" ] || fail "trusted key path is missing"
[ -n "${UML_KERNEL_TRUSTED_KEY_SHA256:-}" ] || fail "trusted key hash is missing"
[ -n "${UML_KERNEL_UPSTREAM_TAG:-}" ] || fail "upstream release tag is missing"
[ -n "${UML_KERNEL_UPSTREAM_TAG_COMMIT:-}" ] ||
	fail "upstream release tag commit is missing"
[ -n "${UML_KERNEL_UPSTREAM_TAG_TREE:-}" ] ||
	fail "upstream release tag tree is missing"
[ -n "${UML_KERNEL_PACKAGED_TREE:-}" ] || fail "packaged tree is missing"
[ -n "${UML_KERNEL_BASE_REF:-}" ] || fail "local base ref is missing"

validate_sha256()
{
	label=$1
	digest=$2
	case "$digest" in
	*[!0-9a-f]*|'') fail "$label is not lowercase SHA-256" ;;
	esac
	[ "${#digest}" -eq 64 ] || fail "$label is not SHA-256"
}

validate_sha256 "archive hash" "$UML_KERNEL_ARCHIVE_SHA256"
validate_sha256 "signature hash" "$UML_KERNEL_SIGNATURE_SHA256"
validate_sha256 "trusted key hash" "$UML_KERNEL_TRUSTED_KEY_SHA256"
validate_git_oid()
{
	label=$1
	digest=$2
	case "$digest" in
	*[!0-9a-f]*|'') fail "$label is not lowercase hexadecimal" ;;
	esac
	[ "${#digest}" -eq 40 ] || fail "$label is not a SHA-1 Git object ID"
}
validate_git_oid "upstream tag commit" "$UML_KERNEL_UPSTREAM_TAG_COMMIT"
validate_git_oid "upstream tag tree" "$UML_KERNEL_UPSTREAM_TAG_TREE"
validate_git_oid "packaged tree" "$UML_KERNEL_PACKAGED_TREE"
case "$UML_KERNEL_SIGNER_FINGERPRINT" in
*[!0-9A-F]*|'') fail "signer fingerprint is not uppercase hexadecimal" ;;
esac
[ "${#UML_KERNEL_SIGNER_FINGERPRINT}" -eq 40 ] ||
	fail "signer fingerprint is not a full OpenPGP fingerprint"
case "$UML_KERNEL_ARCHIVE_URL:$UML_KERNEL_SIGNATURE_URL" in
https://*:https://*) ;;
*) fail "release archive and signature URLs must use HTTPS" ;;
esac
case "$UML_KERNEL_ARCHIVE_FILE:$UML_KERNEL_SIGNATURE_FILE" in
*/*|*..*|:*|'') fail "unsafe archive or signature filename" ;;
esac
case "$UML_KERNEL_ARCHIVE_ROOT" in
*/*|*\\*|..|''|*[!A-Za-z0-9._+-]*) fail "unsafe archive root" ;;
esac
case "$UML_KERNEL_BASE_REF" in
refs/heads/*) ;;
*) fail "local base ref must be below refs/heads" ;;
esac

command -v git >/dev/null 2>&1 || fail "git build tool is required"
command -v shasum >/dev/null 2>&1 || fail "shasum build tool is required"
GPG_COMMAND=${UML_KERNEL_GPG:-gpg}
TAR_COMMAND=${UML_KERNEL_TAR:-tar}
XZ_COMMAND=${UML_KERNEL_XZ:-xz}
command -v "$GPG_COMMAND" >/dev/null 2>&1 ||
	fail "GnuPG is required to authenticate the Linux release"
command -v "$TAR_COMMAND" >/dev/null 2>&1 || fail "tar build tool is required"
command -v "$XZ_COMMAND" >/dev/null 2>&1 || fail "xz build tool is required"
git check-ref-format "$UML_KERNEL_BASE_REF" >/dev/null 2>&1 ||
	fail "invalid local base ref: $UML_KERNEL_BASE_REF"

PARENT_DIR=$(dirname -- "$SOURCE_DIR")
SOURCE_NAME=$(basename -- "$SOURCE_DIR")
mkdir -p -- "$PARENT_DIR" "$CACHE_ROOT"
PARENT_DIR=$(CDPATH= cd -- "$PARENT_DIR" && pwd -P)
CACHE_ROOT=$(CDPATH= cd -- "$CACHE_ROOT" && pwd -P)
[ -d "$GPG_TEMP_ROOT" ] ||
	fail "GnuPG temporary root is not a directory: $GPG_TEMP_ROOT"
GPG_TEMP_ROOT=$(CDPATH= cd -- "$GPG_TEMP_ROOT" && pwd -P)
SOURCE_DIR="$PARENT_DIR/$SOURCE_NAME"
[ ! -e "$SOURCE_DIR" ] && [ ! -L "$SOURCE_DIR" ] ||
	fail "destination already exists: $SOURCE_DIR"

case "$UML_KERNEL_TRUSTED_KEY" in
/*) TRUSTED_KEY=$UML_KERNEL_TRUSTED_KEY ;;
*) TRUSTED_KEY="$ROOT_DIR/$UML_KERNEL_TRUSTED_KEY" ;;
esac
[ -f "$TRUSTED_KEY" ] || fail "trusted signing key is missing: $TRUSTED_KEY"

sha256_file()
{
	shasum -a 256 "$1" | awk '{print $1}'
}

actual_key_sha256=$(sha256_file "$TRUSTED_KEY")
[ "$actual_key_sha256" = "$UML_KERNEL_TRUSTED_KEY_SHA256" ] ||
	fail "trusted signing key hash is $actual_key_sha256, expected $UML_KERNEL_TRUSTED_KEY_SHA256"

cleanup()
{
	if [ -n "$KEYRING" ] && [ -d "$KEYRING" ]; then
		case "$KEYRING" in
		"$GPG_TEMP_ROOT"/uml-kernel-keyring.*) rm -rf -- "$KEYRING" ;;
		esac
	fi
	if [ -n "$DOWNLOAD_TMP" ] && [ -f "$DOWNLOAD_TMP" ]; then
		rm -f -- "$DOWNLOAD_TMP"
	fi
	if [ -n "$WORK_ROOT" ] && [ -d "$WORK_ROOT" ]; then
		case "$WORK_ROOT" in
		"$PARENT_DIR"/.uml-kernel-source.*) rm -rf -- "$WORK_ROOT" ;;
		esac
	fi
}
trap cleanup 0 HUP INT TERM

obtain_file()
{
	label=$1
	filename=$2
	url=$3
	expected_sha256=$4
	path="$CACHE_ROOT/$filename"
	if [ ! -f "$path" ]; then
		[ "$FETCH" -eq 1 ] || fail "$label is absent from offline cache: $path"
		command -v curl >/dev/null 2>&1 || fail "curl is required to fetch release files"
		DOWNLOAD_TMP=$(mktemp "$CACHE_ROOT/.${filename}.download.XXXXXX")
		if ! curl --fail --location --proto '=https' --tlsv1.2 --retry 3 \
			--output "$DOWNLOAD_TMP" "$url"; then
			fail "download failed for $label"
		fi
		actual_sha256=$(sha256_file "$DOWNLOAD_TMP")
		[ "$actual_sha256" = "$expected_sha256" ] ||
			fail "$label download hash is $actual_sha256, expected $expected_sha256"
		mv -- "$DOWNLOAD_TMP" "$path"
		DOWNLOAD_TMP=''
	fi
	actual_sha256=$(sha256_file "$path")
	[ "$actual_sha256" = "$expected_sha256" ] ||
		fail "$label cache hash is $actual_sha256, expected $expected_sha256"
	OBTAINED_PATH=$path
}

OBTAINED_PATH=''
obtain_file "Linux release archive" "$UML_KERNEL_ARCHIVE_FILE" \
	"$UML_KERNEL_ARCHIVE_URL" "$UML_KERNEL_ARCHIVE_SHA256"
ARCHIVE_PATH=$OBTAINED_PATH
obtain_file "Linux release signature" "$UML_KERNEL_SIGNATURE_FILE" \
	"$UML_KERNEL_SIGNATURE_URL" "$UML_KERNEL_SIGNATURE_SHA256"
SIGNATURE_PATH=$OBTAINED_PATH

WORK_ROOT=$(mktemp -d "$PARENT_DIR/.uml-kernel-source.XXXXXX") ||
	fail "cannot create source staging directory below $PARENT_DIR"
# Keep the isolated GnuPG home on a short path. GnuPG may contact an agent even
# for a public-key import, and deeply nested replay roots can otherwise exceed
# the platform's Unix-domain socket path limit.
KEYRING=$(mktemp -d "$GPG_TEMP_ROOT/uml-kernel-keyring.XXXXXX") ||
	fail "cannot create an isolated GnuPG home below $GPG_TEMP_ROOT"
chmod 700 "$KEYRING"
STATUS_PATH="$WORK_ROOT/signature.status"
UNPACK_ROOT="$WORK_ROOT/unpack"
mkdir -- "$UNPACK_ROOT"

if ! "$GPG_COMMAND" --batch --no-options --no-autostart --homedir "$KEYRING" \
	--no-auto-key-retrieve --import "$TRUSTED_KEY" >/dev/null 2>&1; then
	fail "could not import the pinned Linux release signing key"
fi
if ! "$XZ_COMMAND" -cd "$ARCHIVE_PATH" |
	"$GPG_COMMAND" --batch --no-options --no-autostart --homedir "$KEYRING" \
		--no-auto-key-retrieve --status-fd 3 \
		--verify "$SIGNATURE_PATH" - 3>"$STATUS_PATH"; then
	fail "Linux release signature verification failed"
fi
if ! awk -v expected="$UML_KERNEL_SIGNER_FINGERPRINT" '
	$1 == "[GNUPG:]" && $2 == "VALIDSIG" &&
	($3 == expected || $12 == expected) { valid = 1 }
	END { exit valid ? 0 : 1 }
' "$STATUS_PATH"; then
	fail "Linux release signature was not made by $UML_KERNEL_SIGNER_FINGERPRINT"
fi

if ! "$TAR_COMMAND" -xf "$ARCHIVE_PATH" -C "$UNPACK_ROOT"; then
	fail "could not extract the authenticated Linux release archive"
fi
EXTRACTED_SOURCE="$UNPACK_ROOT/$UML_KERNEL_ARCHIVE_ROOT"
[ -d "$EXTRACTED_SOURCE" ] && [ ! -L "$EXTRACTED_SOURCE" ] ||
	fail "archive does not contain expected root $UML_KERNEL_ARCHIVE_ROOT"
unexpected_root=$(find "$UNPACK_ROOT" -mindepth 1 -maxdepth 1 \
	! -name "$UML_KERNEL_ARCHIVE_ROOT" -print -quit)
[ -z "$unexpected_root" ] || fail "archive contains an unexpected top-level path"
[ ! -e "$EXTRACTED_SOURCE/.git" ] && [ ! -L "$EXTRACTED_SOURCE/.git" ] ||
	fail "release archive unexpectedly contains Git metadata"

# Ignore user/system Git filters, hooks, identity, and default-object-format
# settings while deriving the packaged source identity.
isolated_git()
{
	GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null git "$@"
}

isolated_git -c init.templateDir= init -q "$EXTRACTED_SOURCE"
isolated_git -C "$EXTRACTED_SOURCE" add -A
actual_packaged_tree=$(isolated_git -C "$EXTRACTED_SOURCE" write-tree)
[ "$actual_packaged_tree" = "$UML_KERNEL_PACKAGED_TREE" ] ||
	fail "packaged Linux tree is $actual_packaged_tree, expected $UML_KERNEL_PACKAGED_TREE"

# Commit and tagger timestamps are deliberately not synthesized.  They record
# when this local replay repository was generated and are observations, not
# reproducible source identities.
unset GIT_AUTHOR_DATE GIT_COMMITTER_DATE
isolated_git -C "$EXTRACTED_SOURCE" \
	-c user.name='linux-um-darwin source importer' \
	-c user.email='noreply@linux-um-darwin.invalid' \
	-c core.hooksPath=/dev/null commit -q --no-gpg-sign \
	-m "Import Linux $UML_KERNEL_RELEASE_VERSION release archive"
BASE_BRANCH=${UML_KERNEL_BASE_REF#refs/heads/}
isolated_git -C "$EXTRACTED_SOURCE" branch -M "$BASE_BRANCH"

isolated_git -C "$EXTRACTED_SOURCE" config --local \
	uml.source.releaseVersion "$UML_KERNEL_RELEASE_VERSION"
isolated_git -C "$EXTRACTED_SOURCE" config --local \
	uml.source.archiveUrl "$UML_KERNEL_ARCHIVE_URL"
isolated_git -C "$EXTRACTED_SOURCE" config --local \
	uml.source.archiveSha256 "$UML_KERNEL_ARCHIVE_SHA256"
isolated_git -C "$EXTRACTED_SOURCE" config --local \
	uml.source.signatureUrl "$UML_KERNEL_SIGNATURE_URL"
isolated_git -C "$EXTRACTED_SOURCE" config --local \
	uml.source.signatureSha256 "$UML_KERNEL_SIGNATURE_SHA256"
isolated_git -C "$EXTRACTED_SOURCE" config --local \
	uml.source.signerFingerprint "$UML_KERNEL_SIGNER_FINGERPRINT"
isolated_git -C "$EXTRACTED_SOURCE" config --local \
	uml.source.upstreamTag "$UML_KERNEL_UPSTREAM_TAG"
isolated_git -C "$EXTRACTED_SOURCE" config --local \
	uml.source.upstreamTagCommit "$UML_KERNEL_UPSTREAM_TAG_COMMIT"
isolated_git -C "$EXTRACTED_SOURCE" config --local \
	uml.source.upstreamTagTree "$UML_KERNEL_UPSTREAM_TAG_TREE"
isolated_git -C "$EXTRACTED_SOURCE" config --local \
	uml.source.packagedTree "$UML_KERNEL_PACKAGED_TREE"
isolated_git -C "$EXTRACTED_SOURCE" config --local \
	uml.source.baseRef "$UML_KERNEL_BASE_REF"

mv -- "$EXTRACTED_SOURCE" "$SOURCE_DIR"
printf '%s\n' "materialized authenticated Linux $UML_KERNEL_RELEASE_VERSION release in $SOURCE_DIR"
