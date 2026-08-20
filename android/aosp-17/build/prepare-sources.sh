#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Synchronize clean builder-local workspaces to the pinned public source inputs.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
AOSP_METADATA="$ROOT_DIR/android/aosp-17/metadata/aosp.env"
ACK_METADATA="$ROOT_DIR/kernel/android-6.18/patches/metadata.env"

AOSP_SOURCE=
ACK_WORKSPACE=
JOBS=

fail()
{
	printf '%s\n' "android/aosp-17/build/prepare-sources.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: android/aosp-17/build/prepare-sources.sh \
       --aosp-source ABSOLUTE_PATH \
       --ack-workspace ABSOLUTE_PATH [--jobs N]

Run this command locally on the x86_64 Linux builder. It is the only Android
producer step allowed to synchronize public AOSP/ACK source. Both workspaces
must already exist and be clean. No endpoint or transport option is accepted.
EOF
}

absolute_directory()
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
	[ -d "$value" ] || fail "$name does not exist: $value"
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

[ -n "$AOSP_SOURCE" ] || fail "--aosp-source is required"
[ -n "$ACK_WORKSPACE" ] || fail "--ack-workspace is required"
absolute_directory "AOSP source" "$AOSP_SOURCE"
absolute_directory "ACK workspace" "$ACK_WORKSPACE"

[ "$(uname -s)" = Linux ] || fail "builder host must be Linux"
[ "$(uname -m)" = x86_64 ] || fail "builder host must be x86_64"

case "$JOBS" in
'') JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '%s\n' 1) ;;
*[!0-9]*|0) fail "--jobs must be a positive integer" ;;
esac

[ -f "$AOSP_METADATA" ] || fail "missing AOSP metadata: $AOSP_METADATA"
[ -f "$ACK_METADATA" ] || fail "missing ACK metadata: $ACK_METADATA"
# shellcheck disable=SC1090
. "$AOSP_METADATA"
# shellcheck disable=SC1090
. "$ACK_METADATA"

command -v repo >/dev/null 2>&1 || fail "repo is not available on PATH"
command -v git >/dev/null 2>&1 || fail "git is not available on PATH"
[ -d "$AOSP_SOURCE/.repo" ] || fail "AOSP source is not a Repo workspace"
[ -d "$ACK_WORKSPACE/.repo" ] || fail "ACK workspace is not a Repo workspace"
[ -e "$ACK_WORKSPACE/common/.git" ] || fail "ACK common checkout is missing"

dirty_projects=$(dirty_repo_projects)
if [ -n "$dirty_projects" ]; then
	fail "AOSP Repo workspace has dirty projects: $dirty_projects"
fi
if [ -n "$(git -C "$ACK_WORKSPACE/common" status --porcelain)" ]; then
	fail "ACK common checkout is dirty"
fi

(
	cd "$AOSP_SOURCE"
	repo init -u "$ANDROID17_MANIFEST_REPOSITORY" \
		-b "$ANDROID17_MANIFEST_TAG" --manifest-depth=1 \
		--repo-rev=stable --no-repo-verify --no-clone-bundle
	repo sync -c --detach -j "$JOBS"
)

git -C "$ACK_WORKSPACE/common" fetch --no-tags \
	"$UML_KERNEL_UPSTREAM_REPOSITORY" \
	"refs/tags/$UML_KERNEL_UPSTREAM_TAG"
git -C "$ACK_WORKSPACE/common" checkout --detach \
	"$UML_KERNEL_UPSTREAM_COMMIT"

"$SCRIPT_DIR/preflight.sh" \
	--aosp-source "$AOSP_SOURCE" \
	--ack-workspace "$ACK_WORKSPACE" \
	--source-only
