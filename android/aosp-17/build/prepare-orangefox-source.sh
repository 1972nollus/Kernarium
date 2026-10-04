#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Materialize the exact public OrangeFox source inputs on the Linux builder.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ANDROID_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
METADATA="$ANDROID_ROOT/third_party/orangefox/metadata.env"
BASE_MANIFEST="$ANDROID_ROOT/third_party/orangefox/base-manifest.xml"
VERIFIER="$ANDROID_ROOT/tools/verify-orangefox-source.py"

ORANGEFOX_SOURCE=
JOBS=
STAGING=
MANIFEST_OUTPUT=

# shellcheck disable=SC1090
. "$SCRIPT_DIR/common.sh"

usage()
{
	cat <<'EOF'
Usage: android/aosp-17/build/prepare-orangefox-source.sh \
       --orangefox-source ABSOLUTE_PATH [--jobs N]

Run on the x86_64 Linux builder. This is the only OrangeFox producer step
allowed to fetch public source. The destination must be an empty directory or
an exact clean base workspace. The checked resolved manifest pins every Repo
project; the three OrangeFox-owned projects are then pinned to their official
GitLab repositories and exact commits. No endpoint override is accepted.
EOF
}

cleanup()
{
	if [ -n "$STAGING" ] && [ -d "$STAGING" ]; then
		rm -rf -- "$STAGING"
	fi
	if [ -n "$MANIFEST_OUTPUT" ] && [ -f "$MANIFEST_OUTPUT" ]; then
		rm -f -- "$MANIFEST_OUTPUT"
	fi
}
trap cleanup EXIT HUP INT TERM

materialize_external()
{
	destination=$1
	repository=$2
	commit=$3
	tree=$4
	if [ -d "$destination/.git" ] || [ -f "$destination/.git" ]; then
		[ -z "$(git -C "$destination" status --porcelain --untracked-files=all)" ] ||
			fail "external checkout is dirty: $destination"
		git -C "$destination" remote set-url origin "$repository"
		git -C "$destination" fetch --no-tags --depth=1 origin "$commit"
		git -C "$destination" checkout --detach "$commit"
	else
		[ ! -e "$destination" ] || fail "external destination is not a Git checkout: $destination"
		mkdir -p -- "$(dirname -- "$destination")"
		STAGING=$(mktemp -d "$(dirname -- "$destination")/.orangefox.prepare.XXXXXX")
		git -C "$STAGING" init --quiet
		git -C "$STAGING" remote add origin "$repository"
		git -C "$STAGING" fetch --no-tags --depth=1 origin "$commit"
		git -C "$STAGING" checkout --detach "$commit"
		mv -- "$STAGING" "$destination"
		STAGING=
	fi
	actual_commit=$(git -C "$destination" rev-parse HEAD)
	actual_tree=$(git -C "$destination" rev-parse HEAD^{tree})
	[ "$actual_commit" = "$commit" ] || fail "external commit differs: $destination"
	[ "$actual_tree" = "$tree" ] || fail "external tree differs: $destination"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--orangefox-source)
		[ "$#" -ge 2 ] || fail "--orangefox-source requires a path"
		ORANGEFOX_SOURCE=$2
		shift 2
		;;
	--jobs)
		[ "$#" -ge 2 ] || fail "--jobs requires a value"
		JOBS=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$ORANGEFOX_SOURCE" ] || fail "--orangefox-source is required"
absolute_path "OrangeFox source" "$ORANGEFOX_SOURCE"
[ "$(uname -s)" = Linux ] || fail "builder host must be Linux"
[ "$(uname -m)" = x86_64 ] || fail "builder host must be x86_64"
case "$JOBS" in
'') JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '%s\n' 1) ;;
*[!0-9]*|0) fail "--jobs must be a positive integer" ;;
esac
for input in "$METADATA" "$BASE_MANIFEST"; do
	[ -f "$input" ] || fail "required OrangeFox input is missing: $input"
done
[ -x "$VERIFIER" ] || fail "OrangeFox verifier is missing"
command -v repo >/dev/null 2>&1 || fail "repo is not available on PATH"
command -v git >/dev/null 2>&1 || fail "git is not available on PATH"
# shellcheck disable=SC1090
. "$METADATA"
"$VERIFIER" --android-root "$ANDROID_ROOT"

if [ -e "$ORANGEFOX_SOURCE" ]; then
	[ -d "$ORANGEFOX_SOURCE" ] && [ ! -L "$ORANGEFOX_SOURCE" ] ||
		fail "OrangeFox source must be a regular directory"
	if [ ! -d "$ORANGEFOX_SOURCE/.repo" ] &&
		[ -n "$(find "$ORANGEFOX_SOURCE" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
		fail "new OrangeFox source directory must be empty"
	fi
else
	mkdir -p -- "$ORANGEFOX_SOURCE"
fi

if [ ! -d "$ORANGEFOX_SOURCE/.repo" ]; then
	(
		CDPATH= cd -- "$ORANGEFOX_SOURCE"
		repo init -u "$ORANGEFOX_MANIFEST_REPOSITORY" \
			-b "$ORANGEFOX_MANIFEST_BRANCH" --manifest-depth=1 \
			--repo-rev=stable --no-repo-verify --no-clone-bundle
	)
fi
git -C "$ORANGEFOX_SOURCE/.repo/manifests" fetch --no-tags origin \
	"$ORANGEFOX_MANIFEST_COMMIT"
git -C "$ORANGEFOX_SOURCE/.repo/manifests" checkout --detach \
	"$ORANGEFOX_MANIFEST_COMMIT"
(
	CDPATH= cd -- "$ORANGEFOX_SOURCE"
	repo sync -c --detach -j "$JOBS"
)

MANIFEST_OUTPUT=$(mktemp)
(
	CDPATH= cd -- "$ORANGEFOX_SOURCE"
	repo manifest -r
) > "$MANIFEST_OUTPUT"
cmp -s "$MANIFEST_OUTPUT" "$BASE_MANIFEST" ||
	fail "resolved OrangeFox Repo manifest differs from the checked lock"

# bootable/recovery is present in the minimal TWRP manifest, but the official
# OrangeFox sync replaces its transport with the OrangeFox-owned project.
bootable="$ORANGEFOX_SOURCE/bootable/recovery"
bootable_remote=$(git -C "$bootable" remote | head -n 1)
[ -n "$bootable_remote" ] || fail "bootable/recovery has no Git remote"
git -C "$bootable" remote set-url "$bootable_remote" \
	"$ORANGEFOX_BOOTABLE_RECOVERY_REPOSITORY"
git -C "$bootable" fetch --no-tags --depth=1 "$bootable_remote" \
	"$ORANGEFOX_BOOTABLE_RECOVERY_COMMIT"
git -C "$bootable" checkout --detach "$ORANGEFOX_BOOTABLE_RECOVERY_COMMIT"

materialize_external "$ORANGEFOX_SOURCE/vendor/recovery" \
	"$ORANGEFOX_VENDOR_RECOVERY_REPOSITORY" \
	"$ORANGEFOX_VENDOR_RECOVERY_COMMIT" "$ORANGEFOX_VENDOR_RECOVERY_TREE"
materialize_external "$ORANGEFOX_SOURCE/external/se_omapi" \
	"$ORANGEFOX_SE_OMAPI_REPOSITORY" \
	"$ORANGEFOX_SE_OMAPI_COMMIT" "$ORANGEFOX_SE_OMAPI_TREE"

"$VERIFIER" --android-root "$ANDROID_ROOT" \
	--orangefox-source "$ORANGEFOX_SOURCE" --expect base
printf '%s\n' "prepared pinned OrangeFox source: $ORANGEFOX_SOURCE"
