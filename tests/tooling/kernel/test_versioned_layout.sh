#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
COMMON="$ROOT_DIR/kernel/common"
LINE="$ROOT_DIR/kernel/mainline-6.18"
ANDROID_LINE="$ROOT_DIR/kernel/android-6.18"

fail()
{
	printf '%s\n' "kernel versioned layout fixture: $*" >&2
	exit 1
}

for path in \
	"$COMMON/abi/identity.env" \
	"$COMMON/artifacts/kernel-artifact-manifest.schema.json" \
	"$COMMON/artifacts/kernel_artifact.py" \
	"$COMMON/artifacts/verify-kernel-artifact.py" \
	"$COMMON/config/page-16k.fragment" \
	"$COMMON/tooling/verify-series-policy.py" \
	"$LINE/config/common.fragment" \
	"$LINE/config/page-4k.fragment" \
	"$LINE/artifacts/accepted-16k.env" \
	"$LINE/artifacts/accepted-4k.env" \
	"$LINE/artifacts/vulkan-experiment-16k.env" \
	"$LINE/artifacts/vulkan-experiment-4k.env" \
	"$LINE/slices/x86_64/artifacts/accepted-4k.env" \
	"$LINE/patches/metadata.env" \
	"$LINE/build/build-kernel.sh" \
	"$LINE/build/publish-artifact.py" \
	"$LINE/scripts/prepare-source.sh" \
	"$LINE/source-location.conf.example" \
	"$ANDROID_LINE/artifacts/accepted-16k.env" \
	"$ROOT_DIR/android/aosp-17/tools/publish-kernel-artifact.py"; do
	[ -f "$path" ] || fail "required owner is missing: $path"
done

for former in \
	kernel/abi kernel/config kernel/patches kernel/toolchain \
	kernel/build-kernel.sh kernel/prepare-source.sh kernel/materialize-git.sh \
	kernel/audit-delta.sh kernel/rebase-patches.sh kernel/verify-replay.sh \
	kernel/check-next-version.sh kernel/source-location.sh \
	kernel/source-location.conf.example kernel/verify-series-policy.py; do
	[ ! -e "$ROOT_DIR/$former" ] ||
		fail "former singleton owner remains in the worktree: $former"
done

[ ! -d "$COMMON/patches" ] ||
	fail "shared contracts contain a generated patch-series owner"
[ ! -f "$COMMON/config/common.fragment" ] ||
	fail "shared contracts contain a complete product configuration"

grep -F 'kernel/mainline-6.18' "$ROOT_DIR/kernel/CMakeLists.txt" >/dev/null ||
	fail "kernel CMake does not select the current versioned line explicitly"
grep -F 'kernel/common' "$ROOT_DIR/kernel/CMakeLists.txt" >/dev/null ||
	fail "kernel CMake does not consume shared contracts explicitly"

"$COMMON/tooling/verify-series-policy.py" \
	--line-root "$LINE" manifest >/dev/null

if grep -E 'retest-and-upstream|upstream-with-port|pending-upstream' \
	"$LINE/patches/path-ownership.tsv" \
	"$LINE/patches/outside-port-justification.tsv" \
	"$LINE/patches/extraction-decisions.tsv" >/dev/null; then
	fail "active kernel policy still suggests an external contribution action"
fi

printf '%s\n' "kernel versioned ownership layout passed"
