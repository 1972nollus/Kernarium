#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Verify the prepared Android producer host and its local source/tool inputs.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
AOSP_METADATA="$ROOT_DIR/android/aosp-17/metadata/aosp.env"
ACK_METADATA="$ROOT_DIR/kernel/android-6.18/patches/metadata.env"
JELLY_VERIFIER="$ROOT_DIR/android/aosp-17/tools/verify-jelly-source.py"

AOSP_SOURCE=
ACK_WORKSPACE=
DARWIN_SDK=
WORK_ROOT=
RELEASE_ROOT=
SOURCE_ONLY=0

fail()
{
	printf '%s\n' "android/aosp-17/build/preflight.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: android/aosp-17/build/preflight.sh \
       --aosp-source ABSOLUTE_PATH \
       --ack-workspace ABSOLUTE_PATH \
       [--source-only | \
        --darwin-sdk ABSOLUTE_PATH \
        --work-root ABSOLUTE_PATH \
        --release-root ABSOLUTE_PATH]

Run locally on the x86_64 Linux builder after source preparation. Source-only
mode verifies exact source identities. Full mode additionally verifies the
Darwin SDK, pinned checked-out toolchains, and writable builder-local roots.
EOF
}

absolute_path()
{
	name=$1
	value=$2
	case "$value" in
	/*) ;;
	*) fail "$name must be an absolute local path" ;;
	esac
	case "$value" in
	/|*/../*|*/..|*/./*|*/.) fail "$name must be normalized" ;;
	esac
}

require_git_identity()
{
	repository=$1
	expected_commit=$2
	expected_tree=$3
	label=$4
	[ -e "$repository/.git" ] || [ -d "$repository/.git" ] ||
		fail "$label is not a Git checkout: $repository"
	actual_commit=$(git -C "$repository" rev-parse HEAD)
	[ "$actual_commit" = "$expected_commit" ] ||
		fail "$label commit is $actual_commit, expected $expected_commit"
	actual_tree=$(git -C "$repository" rev-parse HEAD^{tree})
	[ "$actual_tree" = "$expected_tree" ] ||
		fail "$label tree is $actual_tree, expected $expected_tree"
	[ -z "$(git -C "$repository" status --porcelain)" ] ||
		fail "$label checkout is dirty"
}

dirty_repo_projects()
{
	(
		cd "$AOSP_SOURCE"
		repo forall -c '
			if test -n "$(git status --porcelain)"; then
				printf "%s\n" "$REPO_PATH"
			fi
		'
	)
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--aosp-source)
		[ "$#" -ge 2 ] || fail "--aosp-source requires a path"
		AOSP_SOURCE=$2
		shift 2
		;;
	--ack-workspace)
		[ "$#" -ge 2 ] || fail "--ack-workspace requires a path"
		ACK_WORKSPACE=$2
		shift 2
		;;
	--darwin-sdk)
		[ "$#" -ge 2 ] || fail "--darwin-sdk requires a path"
		DARWIN_SDK=$2
		shift 2
		;;
	--work-root)
		[ "$#" -ge 2 ] || fail "--work-root requires a path"
		WORK_ROOT=$2
		shift 2
		;;
	--release-root)
		[ "$#" -ge 2 ] || fail "--release-root requires a path"
		RELEASE_ROOT=$2
		shift 2
		;;
	--source-only)
		SOURCE_ONLY=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$AOSP_SOURCE" ] || fail "--aosp-source is required"
[ -n "$ACK_WORKSPACE" ] || fail "--ack-workspace is required"
absolute_path "AOSP source" "$AOSP_SOURCE"
absolute_path "ACK workspace" "$ACK_WORKSPACE"
[ -d "$AOSP_SOURCE" ] || fail "AOSP source does not exist: $AOSP_SOURCE"
[ -d "$ACK_WORKSPACE" ] || fail "ACK workspace does not exist: $ACK_WORKSPACE"

[ "$(uname -s)" = Linux ] || fail "builder host must be Linux"
[ "$(uname -m)" = x86_64 ] || fail "builder host must be x86_64"
[ -r /proc/meminfo ] || fail "builder memory information is unavailable"

[ -f "$AOSP_METADATA" ] || fail "missing AOSP metadata: $AOSP_METADATA"
[ -f "$ACK_METADATA" ] || fail "missing ACK metadata: $ACK_METADATA"
[ -x "$JELLY_VERIFIER" ] || fail "missing Jelly source verifier: $JELLY_VERIFIER"
# shellcheck disable=SC1090
. "$AOSP_METADATA"
# shellcheck disable=SC1090
. "$ACK_METADATA"

[ "$ANDROID17_MAJOR" -eq 17 ] || fail "AOSP metadata is not Android 17"
[ "$ANDROID17_PAGE_SIZE" -eq 16384 ] || fail "Android page size is not 16384"
[ "$UML_KERNEL_FAMILY" = android-6.18 ] || fail "wrong ACK kernel family"

command -v git >/dev/null 2>&1 || fail "git is not available on PATH"
command -v repo >/dev/null 2>&1 || fail "repo is not available on PATH"
[ -d "$AOSP_SOURCE/.repo/manifests" ] || fail "AOSP Repo metadata is missing"
[ -e "$ACK_WORKSPACE/common/.git" ] || fail "ACK common checkout is missing"

require_git_identity "$AOSP_SOURCE/.repo/manifests" \
	"$ANDROID17_MANIFEST_COMMIT" "$ANDROID17_MANIFEST_TREE" \
	"AOSP manifest"
require_git_identity "$ACK_WORKSPACE/common" \
	"$UML_KERNEL_UPSTREAM_COMMIT" "$UML_KERNEL_UPSTREAM_TREE" \
	"ACK common"
dirty_projects=$(dirty_repo_projects)
if [ -n "$dirty_projects" ]; then
	fail "AOSP Repo workspace has dirty projects: $dirty_projects"
fi
"$JELLY_VERIFIER" --android-root "$ROOT_DIR/android/aosp-17" \
	--aosp-source "$AOSP_SOURCE"

if [ "$SOURCE_ONLY" -eq 1 ]; then
	[ -z "$DARWIN_SDK$WORK_ROOT$RELEASE_ROOT" ] ||
		fail "--source-only cannot be combined with build paths"
	printf '%s\n' "Android 17 source preflight passed"
	exit 0
fi

command -v zip >/dev/null 2>&1 || fail "zip is not available on PATH"

[ -n "$DARWIN_SDK" ] || fail "--darwin-sdk is required"
[ -n "$WORK_ROOT" ] || fail "--work-root is required"
[ -n "$RELEASE_ROOT" ] || fail "--release-root is required"
absolute_path "Darwin SDK" "$DARWIN_SDK"
absolute_path "work root" "$WORK_ROOT"
absolute_path "release root" "$RELEASE_ROOT"
[ -d "$DARWIN_SDK" ] || fail "Darwin SDK does not exist: $DARWIN_SDK"
[ -f "$DARWIN_SDK/SDKSettings.json" ] ||
	fail "Darwin SDK is missing SDKSettings.json"
[ -d "$WORK_ROOT" ] && [ -w "$WORK_ROOT" ] ||
	fail "work root must exist and be writable: $WORK_ROOT"
[ -d "$RELEASE_ROOT" ] && [ -w "$RELEASE_ROOT" ] ||
	fail "release root must exist and be writable: $RELEASE_ROOT"

ACK_CLANG="$ACK_WORKSPACE/prebuilts/clang/host/linux-x86/clang-$UML_KERNEL_CLANG_VERSION/bin/clang"
ACK_LLD="$ACK_WORKSPACE/prebuilts/clang/host/linux-x86/clang-$UML_KERNEL_CLANG_VERSION/bin/ld.lld"
ACK_MAKE="$ACK_WORKSPACE/prebuilts/build-tools/linux-x86/bin/make"
AOSP_JAVA="$AOSP_SOURCE/prebuilts/jdk/jdk21/linux-x86/bin/java"
AOSP_NINJA="$AOSP_SOURCE/prebuilts/build-tools/linux-x86/bin/ninja"
for tool in "$ACK_CLANG" "$ACK_LLD" "$ACK_MAKE" "$AOSP_JAVA" "$AOSP_NINJA"; do
	[ -x "$tool" ] || fail "required checked-out tool is missing: $tool"
done

"$ACK_CLANG" --print-targets | grep -Eq '^[[:space:]]*aarch64[[:space:]]' ||
	fail "pinned ACK Clang does not provide the AArch64 target"

memory_kib=$(awk '/^MemTotal:/ { print $2; exit }' /proc/meminfo)
[ -n "$memory_kib" ] || fail "cannot read total builder memory"
[ "$memory_kib" -ge 67108864 ] ||
	fail "builder has less than 64 GiB of memory"
available_kib=$(df -Pk "$WORK_ROOT" | awk 'NR == 2 { print $4 }')
[ -n "$available_kib" ] || fail "cannot read work-root free space"
[ "$available_kib" -ge 419430400 ] ||
	fail "work root has less than 400 GiB available"

printf '%s\n' "Android 17 builder preflight passed"
