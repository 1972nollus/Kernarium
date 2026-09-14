#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Synchronize clean builder-local workspaces to the pinned public source inputs.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
AOSP_METADATA="$ROOT_DIR/android/aosp-17/metadata/aosp.env"
ACK_METADATA="$ROOT_DIR/kernel/android-6.18/patches/metadata.env"
JELLY_METADATA="$ROOT_DIR/android/aosp-17/third_party/jelly/metadata.env"
JELLY_VERIFIER="$ROOT_DIR/android/aosp-17/tools/verify-jelly-source.py"
MESA_VENUS_METADATA="$ROOT_DIR/android/aosp-17/third_party/mesa-venus/metadata.env"
MESA_VENUS_VERIFIER="$ROOT_DIR/android/aosp-17/tools/verify-mesa-venus-source.py"
GIT_IDENTITY="$ROOT_DIR/scripts/git-identity.sh"

AOSP_SOURCE=
ACK_WORKSPACE=
JOBS=
JELLY_STAGING=
MESA_VENUS_STAGING=

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
producer step allowed to synchronize public AOSP, ACK, Jelly, and guest Mesa
source. Both Repo workspaces must already exist and be clean. Jelly and the
Venus Vulkan HAL's Mesa source are materialized at their pinned external
commits plus generated local patch series. No endpoint or transport option is
accepted.
EOF
}

cleanup()
{
	if [ -n "${JELLY_STAGING:-}" ] && [ -d "$JELLY_STAGING" ]; then
		rm -rf -- "$JELLY_STAGING"
	fi
	if [ -n "${MESA_VENUS_STAGING:-}" ] && [ -d "$MESA_VENUS_STAGING" ]; then
		rm -rf -- "$MESA_VENUS_STAGING"
	fi
}
trap cleanup EXIT HUP INT TERM

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

prepare_jelly()
{
	destination="$AOSP_SOURCE/$JELLY_DESTINATION"
	if [ -e "$destination" ] || [ -L "$destination" ]; then
		"$JELLY_VERIFIER" --android-root "$ROOT_DIR/android/aosp-17" \
			--aosp-source "$AOSP_SOURCE"
		printf '%s\n' "reusing pinned Jelly checkout: $destination"
		return
	fi

	parent=$(dirname -- "$destination")
	mkdir -p -- "$parent"
	JELLY_STAGING=$(mktemp -d "$parent/.Jelly.prepare.XXXXXX")
	GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$JELLY_STAGING" init --quiet
	GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$JELLY_STAGING" remote add origin \
		"$JELLY_UPSTREAM_REPOSITORY"
	GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$JELLY_STAGING" fetch --no-tags --depth=1 origin \
		"$JELLY_UPSTREAM_COMMIT"
	GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$JELLY_STAGING" switch --detach "$JELLY_UPSTREAM_COMMIT"
	actual_tree=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$JELLY_STAGING" rev-parse HEAD^{tree})
	[ "$actual_tree" = "$JELLY_UPSTREAM_TREE" ] ||
		fail "fetched Jelly tree is $actual_tree, expected $JELLY_UPSTREAM_TREE"

	tab=$(printf '\t')
	while IFS="$tab" read -r order filename patch_id digest subject; do
		case "$order" in
		\#*|order|'') continue ;;
		esac
		patch="$ROOT_DIR/android/aosp-17/third_party/jelly/series/$filename"
		GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
			git -C "$JELLY_STAGING" \
			-c core.hooksPath=/dev/null \
			-c user.useConfigOnly=true \
			am --committer-date-is-author-date "$patch"
	done < "$ROOT_DIR/android/aosp-17/third_party/jelly/series/manifest.tsv"

	actual_commit=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$JELLY_STAGING" rev-parse HEAD)
	actual_tree=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$JELLY_STAGING" rev-parse HEAD^{tree})
	[ "$actual_commit" = "$JELLY_FINAL_COMMIT" ] ||
		fail "replayed Jelly commit is $actual_commit, expected $JELLY_FINAL_COMMIT"
	[ "$actual_tree" = "$JELLY_FINAL_TREE" ] ||
		fail "replayed Jelly tree is $actual_tree, expected $JELLY_FINAL_TREE"
	mv -- "$JELLY_STAGING" "$destination"
	JELLY_STAGING=
	"$JELLY_VERIFIER" --android-root "$ROOT_DIR/android/aosp-17" \
		--aosp-source "$AOSP_SOURCE"
}

prepare_mesa_venus()
{
	destination="$AOSP_SOURCE/$MESA_VENUS_DESTINATION"
	if [ -e "$destination" ] || [ -L "$destination" ]; then
		"$MESA_VENUS_VERIFIER" --android-root "$ROOT_DIR/android/aosp-17" \
			--aosp-source "$AOSP_SOURCE"
		printf '%s\n' "reusing pinned Mesa Venus checkout: $destination"
		return
	fi

	parent=$(dirname -- "$destination")
	mkdir -p -- "$parent"
	MESA_VENUS_STAGING=$(mktemp -d "$parent/.mesa-venus.prepare.XXXXXX")
	GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$MESA_VENUS_STAGING" init --quiet
	GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$MESA_VENUS_STAGING" remote add origin \
		"$MESA_VENUS_UPSTREAM_REPOSITORY"
	GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$MESA_VENUS_STAGING" fetch --no-tags --depth=1 origin \
		"$MESA_VENUS_UPSTREAM_COMMIT"
	GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$MESA_VENUS_STAGING" switch --create \
		"$MESA_VENUS_PATCH_BRANCH" "$MESA_VENUS_UPSTREAM_COMMIT"
	actual_tree=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$MESA_VENUS_STAGING" rev-parse HEAD^{tree})
	[ "$actual_tree" = "$MESA_VENUS_UPSTREAM_TREE" ] ||
		fail "fetched Mesa tree is $actual_tree, expected $MESA_VENUS_UPSTREAM_TREE"

	tab=$(printf '\t')
	while IFS="$tab" read -r order filename patch_id digest subject; do
		case "$order" in
		\#*|order|'') continue ;;
		esac
		patch="$ROOT_DIR/$MESA_VENUS_GUEST_SERIES/$filename"
		GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
			git -C "$MESA_VENUS_STAGING" \
			-c core.hooksPath=/dev/null \
			-c user.useConfigOnly=true \
			am --committer-date-is-author-date "$patch"
	done < "$ROOT_DIR/$MESA_VENUS_GUEST_SERIES/series.tsv"
	actual_tree=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$MESA_VENUS_STAGING" rev-parse HEAD^{tree})
	[ "$actual_tree" = "$MESA_VENUS_GUEST_TREE" ] ||
		fail "replayed guest Mesa tree is $actual_tree, expected $MESA_VENUS_GUEST_TREE"

	while IFS="$tab" read -r order filename patch_id digest subject; do
		case "$order" in
		\#*|order|'') continue ;;
		esac
		patch="$ROOT_DIR/android/aosp-17/third_party/mesa-venus/series/$filename"
		GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
			git -C "$MESA_VENUS_STAGING" \
			-c core.hooksPath=/dev/null \
			-c user.useConfigOnly=true \
			am --committer-date-is-author-date "$patch"
	done < "$ROOT_DIR/android/aosp-17/third_party/mesa-venus/series/manifest.tsv"
	actual_tree=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
		git -C "$MESA_VENUS_STAGING" rev-parse HEAD^{tree})
	[ "$actual_tree" = "$MESA_VENUS_FINAL_TREE" ] ||
		fail "replayed Mesa Venus tree is $actual_tree, expected $MESA_VENUS_FINAL_TREE"

	mv -- "$MESA_VENUS_STAGING" "$destination"
	MESA_VENUS_STAGING=
	"$MESA_VENUS_VERIFIER" --android-root "$ROOT_DIR/android/aosp-17" \
		--aosp-source "$AOSP_SOURCE"
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
[ -f "$JELLY_METADATA" ] || fail "missing Jelly metadata: $JELLY_METADATA"
[ -x "$JELLY_VERIFIER" ] || fail "missing Jelly source verifier: $JELLY_VERIFIER"
[ -f "$MESA_VENUS_METADATA" ] ||
	fail "missing Mesa Venus metadata: $MESA_VENUS_METADATA"
[ -x "$MESA_VENUS_VERIFIER" ] ||
	fail "missing Mesa Venus source verifier: $MESA_VENUS_VERIFIER"
[ -f "$GIT_IDENTITY" ] || fail "missing Git identity helper: $GIT_IDENTITY"
# shellcheck disable=SC1090
. "$GIT_IDENTITY"
uml_require_git_identity 'android/aosp-17/build/prepare-sources.sh'
# shellcheck disable=SC1090
. "$AOSP_METADATA"
# shellcheck disable=SC1090
. "$ACK_METADATA"
# shellcheck disable=SC1090
. "$JELLY_METADATA"
# shellcheck disable=SC1090
. "$MESA_VENUS_METADATA"

"$JELLY_VERIFIER" --android-root "$ROOT_DIR/android/aosp-17"
"$MESA_VENUS_VERIFIER" --android-root "$ROOT_DIR/android/aosp-17"

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

prepare_jelly
prepare_mesa_venus

"$SCRIPT_DIR/preflight.sh" \
	--aosp-source "$AOSP_SOURCE" \
	--ack-workspace "$ACK_WORKSPACE" \
	--source-only
