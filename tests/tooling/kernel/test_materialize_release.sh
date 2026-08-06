#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
MATERIALIZER="$ROOT_DIR/kernel/materialize-release.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-kernel-release.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' 0 HUP INT TERM

fail()
{
	printf '%s\n' "test-materialize-release.sh: $*" >&2
	exit 1
}

sha256_file()
{
	shasum -a 256 "$1" | awk '{print $1}'
}

expect_failure()
{
	description=$1
	shift
	FAILURE_NUMBER=$((FAILURE_NUMBER + 1))
	if "$@" >"$TEST_ROOT/rejected-$FAILURE_NUMBER.out" \
		2>"$TEST_ROOT/rejected-$FAILURE_NUMBER.err"; then
		fail "$description was incorrectly accepted"
	fi
	[ -s "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" ] ||
		fail "$description was rejected without a diagnostic"
}

CACHE="$TEST_ROOT/cache"
INPUT="$TEST_ROOT/input"
REFERENCE="$TEST_ROOT/reference"
KEY="$TEST_ROOT/release-key.asc"
METADATA="$TEST_ROOT/metadata.env"
FAKE_GPG="$TEST_ROOT/fake-gpg"
GPG_TEMP_ROOT="$TEST_ROOT/gpg-temp"
ARCHIVE_FILE=linux-1.2.3.tar.xz
SIGNATURE_FILE=linux-1.2.3.tar.sign
ARCHIVE_ROOT=linux-1.2.3
FINGERPRINT=1234567890ABCDEF1234567890ABCDEF12345678
BASE_REF=refs/heads/uml/source-linux-1.2.3
FAILURE_NUMBER=0

mkdir -p "$CACHE" "$INPUT/$ARCHIVE_ROOT/docs" "$REFERENCE" "$GPG_TEMP_ROOT"
GPG_TEMP_ROOT=$(CDPATH= cd -- "$GPG_TEMP_ROOT" && pwd -P)
printf '%s\n' 'VERSION = 1' >"$INPUT/$ARCHIVE_ROOT/Makefile"
printf '%s\n' 'release payload' >"$INPUT/$ARCHIVE_ROOT/docs/payload.txt"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$INPUT/$ARCHIVE_ROOT/tool.sh"
chmod +x "$INPUT/$ARCHIVE_ROOT/tool.sh"
ln -s docs/payload.txt "$INPUT/$ARCHIVE_ROOT/payload-link"
COPYFILE_DISABLE=1 tar -C "$INPUT" -cJf "$CACHE/$ARCHIVE_FILE" "$ARCHIVE_ROOT"
printf '%s\n' 'detached fixture signature' >"$CACHE/$SIGNATURE_FILE"
printf '%s\n' 'fixture public key' >"$KEY"
cp "$CACHE/$ARCHIVE_FILE" "$CACHE/$ARCHIVE_FILE.original"
cp "$CACHE/$SIGNATURE_FILE" "$CACHE/$SIGNATURE_FILE.original"
cp "$KEY" "$KEY.original"

tar -xf "$CACHE/$ARCHIVE_FILE" -C "$REFERENCE"
GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
	git -c init.templateDir= init -q "$REFERENCE/$ARCHIVE_ROOT"
GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
	git -C "$REFERENCE/$ARCHIVE_ROOT" add -A
PACKAGED_TREE=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
	git -C "$REFERENCE/$ARCHIVE_ROOT" write-tree)

ARCHIVE_SHA256=$(sha256_file "$CACHE/$ARCHIVE_FILE")
SIGNATURE_SHA256=$(sha256_file "$CACHE/$SIGNATURE_FILE")
KEY_SHA256=$(sha256_file "$KEY")
cat >"$METADATA" <<EOF
UML_KERNEL_SERIES_SCHEMA=2
UML_KERNEL_RELEASE_VERSION='1.2.3'
UML_KERNEL_ARCHIVE_URL='https://example.invalid/linux-1.2.3.tar.xz'
UML_KERNEL_ARCHIVE_FILE='$ARCHIVE_FILE'
UML_KERNEL_ARCHIVE_SHA256='$ARCHIVE_SHA256'
UML_KERNEL_ARCHIVE_ROOT='$ARCHIVE_ROOT'
UML_KERNEL_SIGNATURE_URL='https://example.invalid/linux-1.2.3.tar.sign'
UML_KERNEL_SIGNATURE_FILE='$SIGNATURE_FILE'
UML_KERNEL_SIGNATURE_SHA256='$SIGNATURE_SHA256'
UML_KERNEL_SIGNER_FINGERPRINT='$FINGERPRINT'
UML_KERNEL_TRUSTED_KEY='$KEY'
UML_KERNEL_TRUSTED_KEY_SHA256='$KEY_SHA256'
UML_KERNEL_UPSTREAM_TAG='v1.2.3'
UML_KERNEL_UPSTREAM_TAG_COMMIT='1111111111111111111111111111111111111111'
UML_KERNEL_UPSTREAM_TAG_TREE='2222222222222222222222222222222222222222'
UML_KERNEL_PACKAGED_TREE='$PACKAGED_TREE'
UML_KERNEL_BASE_REF='$BASE_REF'
EOF

cat >"$FAKE_GPG" <<'EOF'
#!/bin/sh
set -eu
previous=''
homedir=''
for argument do
	if [ "$previous" = --homedir ]; then
		homedir=$argument
		break
	fi
	previous=$argument
done
case "$homedir" in
"$FAKE_GPG_EXPECT_HOME_PREFIX"*) ;;
*) exit 64 ;;
esac
case " $* " in
*' --verify '*)
	cat >/dev/null
	[ "${FAKE_GPG_FAIL:-0}" -eq 0 ] || exit 1
	fingerprint=${FAKE_GPG_FINGERPRINT:-1234567890ABCDEF1234567890ABCDEF12345678}
	printf '[GNUPG:] VALIDSIG %s 2026-01-01 0 4 0 1 10 00 %s\n' \
		"$fingerprint" "$fingerprint" >&3
	;;
*)
	# Public-key import for this hermetic fixture.
	exit 0
	;;
esac
EOF
chmod +x "$FAKE_GPG"

materialize_with_metadata()
{
	metadata=$1
	destination=$2
	cache=${3:-$CACHE}
	GIT_AUTHOR_DATE="${GIT_AUTHOR_DATE:-}" \
	GIT_COMMITTER_DATE="${GIT_COMMITTER_DATE:-}" \
	UML_KERNEL_METADATA_PATH="$metadata" \
	UML_KERNEL_GPG="$FAKE_GPG" \
	UML_KERNEL_GPG_TEMP_ROOT="$GPG_TEMP_ROOT" \
	FAKE_GPG_EXPECT_HOME_PREFIX="$GPG_TEMP_ROOT/uml-kernel-keyring." \
	FAKE_GPG_FAIL="${FAKE_GPG_FAIL:-0}" \
	FAKE_GPG_FINGERPRINT="${FAKE_GPG_FINGERPRINT:-$FINGERPRINT}" \
		"$MATERIALIZER" --source "$destination" --cache "$cache" --no-fetch
}

materialize()
{
	materialize_with_metadata "$METADATA" "$1"
}

SOURCE="$TEST_ROOT/source"
START_TIME=$(date +%s)
GIT_AUTHOR_DATE=946684800 GIT_COMMITTER_DATE=946684800 materialize "$SOURCE" >/dev/null
END_TIME=$(date +%s)

[ "$(git -C "$SOURCE" branch --show-current)" = "${BASE_REF#refs/heads/}" ] ||
	fail "materialized checkout is not on the local packaged-source branch"
[ "$(git -C "$SOURCE" rev-parse 'HEAD^{tree}')" = "$PACKAGED_TREE" ] ||
	fail "materialized checkout tree changed"
set -- $(git -C "$SOURCE" rev-list --parents -n 1 HEAD)
[ "$#" -eq 1 ] || fail "archive import commit unexpectedly has a parent"
AUTHOR_TIME=$(git -C "$SOURCE" show -s --format=%at HEAD)
COMMITTER_TIME=$(git -C "$SOURCE" show -s --format=%ct HEAD)
[ "$AUTHOR_TIME" -ge "$START_TIME" ] && [ "$AUTHOR_TIME" -le "$END_TIME" ] ||
	fail "archive import used a synthesized author timestamp"
[ "$COMMITTER_TIME" -ge "$START_TIME" ] && [ "$COMMITTER_TIME" -le "$END_TIME" ] ||
	fail "archive import used a synthesized committer timestamp"
[ "$(git -C "$SOURCE" show -s --format=%s HEAD)" = \
	"Import Linux 1.2.3 release archive" ] || fail "unexpected import subject"
[ -z "$(git -C "$SOURCE" status --porcelain --untracked-files=all)" ] ||
	fail "materialized checkout is dirty"
[ "$(git -C "$SOURCE" config --local --get uml.source.archiveSha256)" = \
	"$ARCHIVE_SHA256" ] || fail "archive provenance was not recorded"
[ "$(git -C "$SOURCE" config --local --get uml.source.signatureSha256)" = \
	"$SIGNATURE_SHA256" ] || fail "signature provenance was not recorded"
[ "$(git -C "$SOURCE" config --local --get uml.source.signerFingerprint)" = \
	"$FINGERPRINT" ] || fail "signer provenance was not recorded"
[ "$(git -C "$SOURCE" config --local --get uml.source.upstreamTag)" = v1.2.3 ] ||
	fail "upstream tag provenance was not recorded"
[ "$(git -C "$SOURCE" config --local --get uml.source.upstreamTagCommit)" = \
	1111111111111111111111111111111111111111 ] ||
	fail "upstream tag commit provenance was not recorded"
[ "$(git -C "$SOURCE" config --local --get uml.source.upstreamTagTree)" = \
	2222222222222222222222222222222222222222 ] ||
	fail "upstream tag tree provenance was not recorded"

expect_failure "replacement of an existing source checkout" materialize "$SOURCE"

printf '%s\n' tampered >>"$CACHE/$ARCHIVE_FILE"
expect_failure "tampered cached release archive" materialize "$TEST_ROOT/archive-tamper"
[ ! -e "$TEST_ROOT/archive-tamper" ] || fail "archive failure left a source checkout"
cp "$CACHE/$ARCHIVE_FILE.original" "$CACHE/$ARCHIVE_FILE"

printf '%s\n' tampered >>"$CACHE/$SIGNATURE_FILE"
expect_failure "tampered cached release signature" materialize "$TEST_ROOT/signature-tamper"
[ ! -e "$TEST_ROOT/signature-tamper" ] || fail "signature failure left a source checkout"
cp "$CACHE/$SIGNATURE_FILE.original" "$CACHE/$SIGNATURE_FILE"

printf '%s\n' tampered >>"$KEY"
expect_failure "tampered trusted release key" materialize "$TEST_ROOT/key-tamper"
[ ! -e "$TEST_ROOT/key-tamper" ] || fail "key failure left a source checkout"
cp "$KEY.original" "$KEY"

wrong_fingerprint()
{
	FAKE_GPG_FINGERPRINT=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA \
		materialize "$1"
}
expect_failure "signature from an unpinned fingerprint" \
	wrong_fingerprint "$TEST_ROOT/wrong-fingerprint"
[ ! -e "$TEST_ROOT/wrong-fingerprint" ] ||
	fail "fingerprint failure left a source checkout"

bad_signature()
{
	FAKE_GPG_FAIL=1 materialize "$1"
}
expect_failure "cryptographically invalid release signature" \
	bad_signature "$TEST_ROOT/bad-signature"
[ ! -e "$TEST_ROOT/bad-signature" ] ||
	fail "signature verification failure left a source checkout"

BAD_TREE_METADATA="$TEST_ROOT/bad-tree-metadata.env"
sed "s/UML_KERNEL_PACKAGED_TREE='[^']*'/UML_KERNEL_PACKAGED_TREE='0000000000000000000000000000000000000000'/" \
	"$METADATA" >"$BAD_TREE_METADATA"
expect_failure "unexpected packaged source tree" materialize_with_metadata \
	"$BAD_TREE_METADATA" "$TEST_ROOT/bad-tree"
[ ! -e "$TEST_ROOT/bad-tree" ] || fail "tree failure left a source checkout"

EMPTY_CACHE="$TEST_ROOT/empty-cache"
mkdir "$EMPTY_CACHE"
expect_failure "missing offline release files" materialize_with_metadata \
	"$METADATA" "$TEST_ROOT/missing-cache" "$EMPTY_CACHE"
[ ! -e "$TEST_ROOT/missing-cache" ] ||
	fail "missing-cache failure left a source checkout"

[ -z "$(find "$TEST_ROOT" -maxdepth 1 -name '.uml-kernel-source.*' -print -quit)" ] ||
	fail "failed preparation left a staging directory"
[ -z "$(find "$GPG_TEMP_ROOT" -maxdepth 1 -name 'uml-kernel-keyring.*' -print -quit)" ] ||
	fail "preparation left an isolated GnuPG home"

printf '%s\n' "kernel release materialization tests passed"
