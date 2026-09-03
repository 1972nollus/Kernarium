#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
COMMON_METADATA="$LINE_ROOT/patches/metadata.env"
COMMON_SERIES="$LINE_ROOT/patches/series"
SLICE_ROOT="$LINE_ROOT/slices/x86_64"
SLICE_METADATA="$SLICE_ROOT/patches/metadata.env"
SLICE_SERIES="$SLICE_ROOT/patches/series"
SLICE_COMPOSITION="$SLICE_ROOT/patches/composition.tsv"
SERIES_POLICY="$ROOT_DIR/kernel/common/tooling/verify-series-policy.py"
KERNEL_ARTIFACT_MODULE="$ROOT_DIR/kernel/common/artifacts/kernel_artifact.py"
KERNEL_PUBLISHER="$LINE_ROOT/build/publish-artifact.py"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-kernel-slices.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

fail()
{
	printf '%s\n' "kernel slice contract fixture: $*" >&2
	exit 1
}

require_file()
{
	[ -f "$1" ] || fail "required slice owner is missing: $1"
}

manifest_count()
{
	awk -F '\t' '$1 ~ /^[0-9]+$/ { count++ }
		END { print count + 0 }' "$1"
}

provenance_count()
{
	PYTHONPATH=$(dirname -- "$KERNEL_ARTIFACT_MODULE") \
		python3 -B -c \
		'from pathlib import Path
from kernel_artifact import patch_provenance_count
import sys
expected = None
if len(sys.argv) == 5:
    expected = (("common", sys.argv[3]), ("x86_64", sys.argv[4]))
print(patch_provenance_count(
    Path(sys.argv[1]), Path(sys.argv[2]), expected
))' \
		"$@"
}

for path in \
	"$COMMON_METADATA" \
	"$COMMON_SERIES/manifest.tsv" \
	"$LINE_ROOT/config/common.fragment" \
	"$LINE_ROOT/config/page-4k.fragment" \
	"$LINE_ROOT/scripts/select-slice.sh" \
	"$SLICE_ROOT/config/userspace.fragment" \
	"$SLICE_METADATA" \
	"$SLICE_SERIES/manifest.tsv" \
	"$SLICE_COMPOSITION" \
	"$SLICE_ROOT/patches/path-ownership.tsv" \
	"$SLICE_ROOT/patches/outside-port-justification.tsv" \
	"$SLICE_ROOT/patches/extraction-decisions.tsv" \
	"$SLICE_ROOT/patches/topic-path-overlaps.tsv" \
	"$KERNEL_ARTIFACT_MODULE" \
	"$KERNEL_PUBLISHER"; do
	require_file "$path"
done

# The normal arm64 product owns the shared 26-topic base and no compatibility
# topic.  These identifiers name the former mixed-ABI implementation rather
# than ordinary architecture-neutral x86 references in upstream UML code.
# shellcheck disable=SC1090
. "$COMMON_METADATA"
COMMON_PATCH_BRANCH=$UML_KERNEL_PATCH_BRANCH
COMMON_FINAL_TREE=$UML_KERNEL_FINAL_TREE
COMMON_PATCH_COUNT=$UML_KERNEL_PATCH_COUNT
COMMON_RELEASE_VERSION=$UML_KERNEL_RELEASE_VERSION
COMMON_UPSTREAM_REPOSITORY=$UML_KERNEL_UPSTREAM_REPOSITORY
COMMON_UPSTREAM_BRANCH=$UML_KERNEL_UPSTREAM_BRANCH
COMMON_UPSTREAM_COMMIT=$UML_KERNEL_UPSTREAM_COMMIT
COMMON_UPSTREAM_TREE=$UML_KERNEL_UPSTREAM_TREE
COMMON_BASE_REF=$UML_KERNEL_BASE_REF
COMMON_ABI_VERSION=$UML_KERNEL_ABI_VERSION
COMMON_ABI_FINGERPRINT=$UML_KERNEL_ABI_FINGERPRINT
[ "$COMMON_PATCH_COUNT" -eq 26 ] ||
	fail "common series has $COMMON_PATCH_COUNT patches; expected 26"
[ "$(manifest_count "$COMMON_SERIES/manifest.tsv")" -eq 26 ] ||
	fail "common series manifest is not the canonical 26-topic base"
common_patch_files=$(find "$COMMON_SERIES" -maxdepth 1 -type f \
	-name '*.patch' | wc -l | tr -d ' ')
[ "$common_patch_files" -eq 26 ] ||
	fail "common series contains $common_patch_files generated patches; expected 26"
if grep -E -i \
	'UML_X86_64_USERSPACE_ONLY|UPT_IS_X86_64|upt_is_x86_64|UML_GUEST_ISA_X86_64|enum uml_guest_isa|run x86-64 userspace through Darwin gadgets' \
	"$COMMON_SERIES"/*.patch >/dev/null; then
	fail "common arm64 patch series still contains mixed-ABI compatibility code"
fi
if grep -F 'CONFIG_UML_X86_64_USERSPACE_ONLY' \
	"$LINE_ROOT/config/common.fragment" \
	"$LINE_ROOT/config/page-4k.fragment" >/dev/null; then
	fail "shared arm64 configuration enables the x86_64 userspace slice"
fi

# The x86_64 slice is a true overlay: it carries only its fixed-ABI delta,
# identifies the common tree as its parent, and reuses both shared config
# fragments instead of cloning them below the slice directory.
grep -F 'CONFIG_UML_X86_64_USERSPACE_ONLY=y' \
	"$SLICE_ROOT/config/userspace.fragment" >/dev/null ||
	fail "x86_64 userspace fragment does not select its fixed-ABI kernel mode"
grep -Fxq 'CONFIG_SYSCALL_USER_DISPATCH=y' \
	"$SLICE_ROOT/config/userspace.fragment" ||
	fail "x86_64 userspace fragment does not require syscall-user-dispatch"
if grep -E 'CONFIG_(BPF_SYSCALL|CGROUP_BPF)' \
	"$SLICE_ROOT/config/userspace.fragment" >/dev/null; then
	fail "x86_64 userspace slice overrides the shared BPF policy"
fi
grep -Fxq 'CONFIG_BPF_SYSCALL=y' "$LINE_ROOT/config/common.fragment" ||
	fail "shared kernel policy does not expose the eBPF syscall"
grep -Fxq 'CONFIG_CGROUP_BPF=y' "$LINE_ROOT/config/common.fragment" ||
	fail "shared kernel policy does not expose cgroup eBPF attachment"
[ ! -e "$SLICE_ROOT/config/common.fragment" ] ||
	fail "x86_64 slice duplicates the shared common config fragment"
[ ! -e "$SLICE_ROOT/config/page-4k.fragment" ] ||
	fail "x86_64 slice duplicates the shared 4 KiB config fragment"
[ ! -e "$SLICE_ROOT/artifacts/accepted-16k.env" ] ||
	fail "x86_64 slice exposes an unsupported 16 KiB artifact"

# shellcheck disable=SC1090
. "$SLICE_METADATA"
[ "${UML_KERNEL_SLICE:-}" = x86_64 ] ||
	fail "x86_64 overlay metadata has the wrong slice identity"
[ "${UML_KERNEL_PARENT_TREE:-}" = "$COMMON_FINAL_TREE" ] ||
	fail "x86_64 overlay is not based on the canonical common tree"
[ "$UML_KERNEL_PATCH_BRANCH" != "$COMMON_PATCH_BRANCH" ] ||
	fail "common and x86_64 patches use the same maintained Git branch"
[ "$UML_KERNEL_PATCH_COUNT" -gt 0 ] ||
	fail "x86_64 overlay has no dedicated patch"
SLICE_PATCH_BRANCH=$UML_KERNEL_PATCH_BRANCH
SLICE_FINAL_TREE=$UML_KERNEL_FINAL_TREE
SLICE_PATCH_COUNT=$UML_KERNEL_PATCH_COUNT
SLICE_ARTIFACT_PATCH_COUNT=${UML_KERNEL_ARTIFACT_PATCH_COUNT:-$SLICE_PATCH_COUNT}
[ "$(manifest_count "$SLICE_SERIES/manifest.tsv")" \
	-eq "$SLICE_PATCH_COUNT" ] ||
	fail "x86_64 overlay manifest count disagrees with its metadata"
slice_patch_files=$(find "$SLICE_SERIES" -maxdepth 1 -type f \
	-name '*.patch' | wc -l | tr -d ' ')
[ "$slice_patch_files" -eq "$SLICE_PATCH_COUNT" ] ||
	fail "x86_64 overlay generated patch count disagrees with its metadata"
if grep -E 'UML_GUEST_ISA_X86_64|padding = UML_GUEST' \
	"$SLICE_SERIES"/*.patch >/dev/null; then
	fail "fixed x86_64 overlay still publishes a per-task ISA tag"
fi
grep -F '#define ELF_ARCH EM_X86_64' "$SLICE_SERIES"/*.patch >/dev/null ||
	fail "x86_64 overlay does not replace the userspace ELF architecture"
grep -F 'arch/x86/entry/syscalls/syscall_64.tbl' \
	"$SLICE_SERIES"/*.patch >/dev/null ||
	fail "x86_64 overlay does not generate its native syscall table"
grep -F 'arch/x86/entry/syscalls/syscall_32.tbl' \
	"$SLICE_SERIES"/*.patch >/dev/null ||
	fail "x86_64 overlay does not generate its i386 syscall table"
grep -F '#define UML_X86_64_NR_SYSCALLS 470' \
	"$SLICE_SERIES"/*.patch >/dev/null ||
	fail "x86_64 syscall table bound does not cover Linux 6.18"
grep -F '#define UML_X86_32_NR_SYSCALLS 470' \
	"$SLICE_SERIES"/*.patch >/dev/null ||
	fail "i386 syscall table bound does not cover Linux 6.18"
grep -F 'uml_x86_64_sys_call_table[UML_X86_64_NR_SYSCALLS]' \
	"$SLICE_SERIES"/*.patch >/dev/null ||
	fail "x86_64 generated syscall dispatch table is missing"
grep -F 'uml_x86_32_sys_call_table[UML_X86_32_NR_SYSCALLS]' \
	"$SLICE_SERIES"/*.patch >/dev/null ||
	fail "i386 generated syscall dispatch table is missing"
grep -F 'select SYSCALL_USER_DISPATCH' \
	"$SLICE_SERIES"/*.patch >/dev/null ||
	fail "x86_64 slice does not select syscall-user-dispatch"
grep -F 'syscall_user_dispatch(regs)' \
	"$SLICE_SERIES"/*.patch >/dev/null ||
	fail "x86_64 UML syscall entry does not apply syscall-user-dispatch"
if awk '
/^\+[^+]/ {
	line = substr($0, 2)
	if (line ~ /uml_x86_(32|64)_syscall_number/)
		balance[line]++
}
/^-[^-]/ {
	line = substr($0, 2)
	if (line ~ /uml_x86_(32|64)_syscall_number/)
		balance[line]--
}
END {
	for (line in balance)
		if (balance[line] > 0)
			exit 0
	exit 1
}' "$SLICE_SERIES"/*.patch; then
	fail "x86 syscall dispatch still uses a sparse number switch"
fi

"$SERIES_POLICY" --line-root "$LINE_ROOT" manifest >/dev/null
"$SERIES_POLICY" --line-root "$SLICE_ROOT" manifest >/dev/null

# The x86_64 artifact provenance is the ordered composition of the common and
# overlay manifests.  The publisher must never fall back to the overlay-only
# manifest, and the common plus overlay count is checked through the same
# parser used during publication.
grep -F \
	'patch_manifest_path = slice_root / "patches/composition.tsv"' \
	"$KERNEL_PUBLISHER" >/dev/null ||
	fail "x86_64 artifact publication does not require composition.tsv"
composed_count=$(provenance_count "$SLICE_COMPOSITION" "$ROOT_DIR" \
	'kernel/mainline-6.18/patches/series/manifest.tsv' \
	'kernel/mainline-6.18/slices/x86_64/patches/series/manifest.tsv')
[ "$composed_count" -eq \
	$((COMMON_PATCH_COUNT + SLICE_PATCH_COUNT)) ] &&
	[ "$composed_count" -eq "$SLICE_ARTIFACT_PATCH_COUNT" ] ||
	fail "x86_64 composition count differs from common+overlay metadata"

# Exercise both referenced series independently. A composition row is not a
# trusted summary: its count and digest must match the referenced manifest.
COMPOSITION_FIXTURE="$TEST_ROOT/composition-fixture"
mkdir -p "$COMPOSITION_FIXTURE/manifests"
ZERO40=0000000000000000000000000000000000000000
ZERO64=0000000000000000000000000000000000000000000000000000000000000000
BAD64=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
{
	printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later'
	printf 'order\tfilename\tstable_patch_id\tsha256\tsubject\n'
	printf '1\t0001-common.patch\t%s\t%s\tcommon fixture\n' \
		"$ZERO40" "$ZERO64"
} > "$COMPOSITION_FIXTURE/manifests/common.tsv"
{
	printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later'
	printf 'order\tfilename\tstable_patch_id\tsha256\tsubject\n'
	printf '1\t0001-overlay.patch\t%s\t%s\toverlay fixture\n' \
		"$ZERO40" "$ZERO64"
} > "$COMPOSITION_FIXTURE/manifests/overlay.tsv"
common_digest=$(shasum -a 256 \
	"$COMPOSITION_FIXTURE/manifests/common.tsv" | awk '{ print $1 }')
overlay_digest=$(shasum -a 256 \
	"$COMPOSITION_FIXTURE/manifests/overlay.tsv" | awk '{ print $1 }')

write_composition_fixture()
{
	destination=$1
	common_count=$2
	requested_common_digest=$3
	overlay_count=$4
	requested_overlay_digest=$5
	{
		printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later'
		printf 'order\tname\tpatch_count\tmanifest_path\tmanifest_sha256\n'
		printf '1\tcommon\t%s\tmanifests/common.tsv\t%s\n' \
			"$common_count" "$requested_common_digest"
		printf '2\tx86_64\t%s\tmanifests/overlay.tsv\t%s\n' \
			"$overlay_count" "$requested_overlay_digest"
	} > "$destination"
}

write_composition_fixture "$COMPOSITION_FIXTURE/valid.tsv" \
	1 "$common_digest" 1 "$overlay_digest"
[ "$(provenance_count "$COMPOSITION_FIXTURE/valid.tsv" \
	"$COMPOSITION_FIXTURE" 'manifests/common.tsv' \
	'manifests/overlay.tsv')" -eq 2 ] ||
	fail "valid common+overlay composition fixture was rejected"
if provenance_count "$COMPOSITION_FIXTURE/missing.tsv" \
	"$COMPOSITION_FIXTURE" >"$TEST_ROOT/composition.out" \
	2>"$TEST_ROOT/composition.err"; then
	fail "missing composition.tsv was accepted"
fi
grep -F 'patch provenance is missing or unsafe' \
	"$TEST_ROOT/composition.err" >/dev/null ||
	fail "missing composition.tsv failed for the wrong reason"
for series in common x86_64; do
	if [ "$series" = common ]; then
		write_composition_fixture "$COMPOSITION_FIXTURE/bad-digest.tsv" \
			1 "$BAD64" 1 "$overlay_digest"
		write_composition_fixture "$COMPOSITION_FIXTURE/bad-count.tsv" \
			2 "$common_digest" 1 "$overlay_digest"
	else
		write_composition_fixture "$COMPOSITION_FIXTURE/bad-digest.tsv" \
			1 "$common_digest" 1 "$BAD64"
		write_composition_fixture "$COMPOSITION_FIXTURE/bad-count.tsv" \
			1 "$common_digest" 2 "$overlay_digest"
	fi
	if provenance_count "$COMPOSITION_FIXTURE/bad-digest.tsv" \
		"$COMPOSITION_FIXTURE" >"$TEST_ROOT/composition.out" \
		2>"$TEST_ROOT/composition.err"; then
		fail "$series composition accepted a stale manifest digest"
	fi
	grep -F "patch composition series $series digest differs" \
		"$TEST_ROOT/composition.err" >/dev/null ||
		fail "$series manifest digest failed for the wrong reason"
	if provenance_count "$COMPOSITION_FIXTURE/bad-count.tsv" \
		"$COMPOSITION_FIXTURE" >"$TEST_ROOT/composition.out" \
		2>"$TEST_ROOT/composition.err"; then
		fail "$series composition accepted a stale manifest count"
	fi
	grep -F "patch composition series $series contains 1 patches, expected 2" \
		"$TEST_ROOT/composition.err" >/dev/null ||
		fail "$series manifest count failed for the wrong reason"
done

{
	printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later'
	printf 'order\tname\tpatch_count\tmanifest_path\tmanifest_sha256\n'
	printf '1\tx86_64\t1\tmanifests/overlay.tsv\t%s\n' "$overlay_digest"
	printf '2\tcommon\t1\tmanifests/common.tsv\t%s\n' "$common_digest"
} > "$COMPOSITION_FIXTURE/swapped.tsv"
if provenance_count "$COMPOSITION_FIXTURE/swapped.tsv" \
	"$COMPOSITION_FIXTURE" 'manifests/common.tsv' 'manifests/overlay.tsv' \
	>"$TEST_ROOT/composition.out" 2>"$TEST_ROOT/composition.err"; then
	fail "x86_64 composition accepted overlay before common"
fi
grep -F 'patch composition differs from its required ordered series' \
	"$TEST_ROOT/composition.err" >/dev/null ||
	fail "swapped x86_64 composition failed for the wrong reason"

cp "$COMPOSITION_FIXTURE/manifests/common.tsv" \
	"$COMPOSITION_FIXTURE/manifests/common-copy.tsv"
cp "$COMPOSITION_FIXTURE/manifests/overlay.tsv" \
	"$COMPOSITION_FIXTURE/manifests/extra.tsv"
{
	printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later'
	printf 'order\tname\tpatch_count\tmanifest_path\tmanifest_sha256\n'
	printf '1\tshared\t1\tmanifests/common.tsv\t%s\n' "$common_digest"
	printf '2\tx86_64\t1\tmanifests/overlay.tsv\t%s\n' "$overlay_digest"
} > "$COMPOSITION_FIXTURE/wrong-name.tsv"
if provenance_count "$COMPOSITION_FIXTURE/wrong-name.tsv" \
	"$COMPOSITION_FIXTURE" 'manifests/common.tsv' 'manifests/overlay.tsv' \
	>"$TEST_ROOT/composition.out" 2>"$TEST_ROOT/composition.err"; then
	fail "x86_64 composition accepted an unexpected series name"
fi
grep -F 'patch composition differs from its required ordered series' \
	"$TEST_ROOT/composition.err" >/dev/null ||
	fail "wrong x86_64 composition name failed for the wrong reason"
{
	printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later'
	printf 'order\tname\tpatch_count\tmanifest_path\tmanifest_sha256\n'
	printf '1\tcommon\t1\tmanifests/common-copy.tsv\t%s\n' "$common_digest"
	printf '2\tx86_64\t1\tmanifests/overlay.tsv\t%s\n' "$overlay_digest"
} > "$COMPOSITION_FIXTURE/wrong-path.tsv"
if provenance_count "$COMPOSITION_FIXTURE/wrong-path.tsv" \
	"$COMPOSITION_FIXTURE" 'manifests/common.tsv' 'manifests/overlay.tsv' \
	>"$TEST_ROOT/composition.out" 2>"$TEST_ROOT/composition.err"; then
	fail "x86_64 composition accepted an unexpected manifest path"
fi
grep -F 'patch composition differs from its required ordered series' \
	"$TEST_ROOT/composition.err" >/dev/null ||
	fail "wrong x86_64 manifest path failed for the wrong reason"
{
	printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later'
	printf 'order\tname\tpatch_count\tmanifest_path\tmanifest_sha256\n'
	printf '1\tcommon\t1\tmanifests/common.tsv\t%s\n' "$common_digest"
	printf '2\tx86_64\t1\tmanifests/overlay.tsv\t%s\n' "$overlay_digest"
	printf '3\textra\t1\tmanifests/extra.tsv\t%s\n' "$overlay_digest"
} > "$COMPOSITION_FIXTURE/extra-row.tsv"
if provenance_count "$COMPOSITION_FIXTURE/extra-row.tsv" \
	"$COMPOSITION_FIXTURE" 'manifests/common.tsv' 'manifests/overlay.tsv' \
	>"$TEST_ROOT/composition.out" 2>"$TEST_ROOT/composition.err"; then
	fail "x86_64 composition accepted an extra series row"
fi
grep -F 'patch composition differs from its required ordered series' \
	"$TEST_ROOT/composition.err" >/dev/null ||
	fail "extra x86_64 composition row failed for the wrong reason"

# shellcheck disable=SC1090
. "$LINE_ROOT/scripts/select-slice.sh"
uml_kernel_select_slice "$LINE_ROOT" arm64 ||
	fail "slice selector cannot load the normal arm64 product"
[ "$UML_KERNEL_POLICY_ROOT" = "$LINE_ROOT" ] &&
	[ "$UML_KERNEL_PATCH_DIR" = "$COMMON_SERIES" ] &&
	[ -z "$UML_KERNEL_SLICE_CONFIG_FRAGMENT" ] ||
	fail "arm64 selection does not resolve to the shared series/config"
uml_kernel_select_slice "$LINE_ROOT" x86_64 ||
	fail "slice selector cannot load the x86_64 overlay"
[ "$UML_KERNEL_POLICY_ROOT" = "$SLICE_ROOT" ] &&
	[ "$UML_KERNEL_PATCH_DIR" = "$SLICE_SERIES" ] &&
	[ "$UML_KERNEL_SLICE_CONFIG_FRAGMENT" = \
		"$SLICE_ROOT/config/userspace.fragment" ] ||
	fail "x86_64 selection does not compose the overlay series/config"

# A stale overlay parent is rejected by every ordinary consumer.  The
# selector's explicit escape hatch exists only so maintenance commands can
# update or recreate the overlay after the shared series changes.
SELECTOR_FIXTURE="$TEST_ROOT/selector-fixture"
STALE_PARENT_TREE=ffffffffffffffffffffffffffffffffffffffff
mkdir -p \
	"$SELECTOR_FIXTURE/patches" \
	"$SELECTOR_FIXTURE/scripts" \
	"$SELECTOR_FIXTURE/slices/x86_64/patches"
cp "$COMMON_METADATA" "$SELECTOR_FIXTURE/patches/metadata.env"
cp "$LINE_ROOT/scripts/select-slice.sh" \
	"$SELECTOR_FIXTURE/scripts/select-slice.sh"
sed \
	"s/^UML_KERNEL_PARENT_TREE=.*/UML_KERNEL_PARENT_TREE='$STALE_PARENT_TREE'/" \
	"$SLICE_METADATA" \
	> "$SELECTOR_FIXTURE/slices/x86_64/patches/metadata.env"
if (
	# shellcheck disable=SC1090
	. "$SELECTOR_FIXTURE/scripts/select-slice.sh"
	uml_kernel_select_slice "$SELECTOR_FIXTURE" x86_64
) >"$TEST_ROOT/selector.out" 2>"$TEST_ROOT/selector.err"; then
	fail "strict slice selection accepted a stale overlay parent"
fi
grep -F 'x86_64 slice parent tree does not match the common series' \
	"$TEST_ROOT/selector.err" >/dev/null ||
	fail "strict stale-parent selection failed for the wrong reason"
(
	# shellcheck disable=SC1090
	. "$SELECTOR_FIXTURE/scripts/select-slice.sh"
	uml_kernel_select_slice \
		"$SELECTOR_FIXTURE" x86_64 allow-stale-parent
	[ "$UML_KERNEL_PARENT_TREE" = "$STALE_PARENT_TREE" ]
) || fail "maintenance selection cannot load stale overlay metadata"
if (
	# shellcheck disable=SC1090
	. "$SELECTOR_FIXTURE/scripts/select-slice.sh"
	uml_kernel_select_slice "$SELECTOR_FIXTURE" x86_64 arbitrary-mode
) >"$TEST_ROOT/selector.out" 2>"$TEST_ROOT/selector.err"; then
	fail "slice selector accepted an unrecognized maintenance mode"
fi
grep -F 'invalid kernel slice parent-selection mode: arbitrary-mode' \
	"$TEST_ROOT/selector.err" >/dev/null ||
	fail "unknown selector maintenance mode failed for the wrong reason"

grep -F 'uml_kernel_select_slice "$LINE_ROOT" "$SLICE" allow-stale-parent' \
	"$LINE_ROOT/scripts/rebase-patches.sh" >/dev/null ||
	fail "overlay rebase cannot select stale-parent metadata for maintenance"
grep -F 'if [ "$SLICE" = x86_64 ] && [ "$RECREATE" -eq 1 ]; then' \
	"$LINE_ROOT/scripts/prepare-source.sh" >/dev/null &&
	grep -F 'SELECT_PARENT_MODE=allow-stale-parent' \
		"$LINE_ROOT/scripts/prepare-source.sh" >/dev/null ||
	fail "stale-parent source preparation is not confined to x86_64 --recreate"
PREPARE_SOURCE="$LINE_ROOT/scripts/prepare-source.sh"
grep -F 'refs/uml/recreate-backups/$UML_KERNEL_SLICE/$old_managed_tip' \
	"$PREPARE_SOURCE" >/dev/null &&
	grep -F 'verify_managed_branch "$CREATE_BRANCH"' \
		"$PREPARE_SOURCE" >/dev/null &&
	grep -F 'switch --detach "$candidate_tip"' \
		"$PREPARE_SOURCE" >/dev/null &&
	grep -F '"refs/heads/$TARGET_PATCH_BRANCH" "$candidate_tip"' \
		"$PREPARE_SOURCE" >/dev/null ||
	fail "recreate does not preserve, verify, and CAS-install its replacement"
if grep -F 'branch -M "$TARGET_PATCH_BRANCH"' \
	"$PREPARE_SOURCE" >/dev/null; then
	fail "recreate still replaces the managed ref without compare-and-swap"
fi
for strict_consumer in \
	"$LINE_ROOT/build/build-kernel.sh" \
	"$LINE_ROOT/scripts/audit-delta.sh" \
	"$LINE_ROOT/scripts/check-next-version.sh"; do
	if grep -F 'allow-stale-parent' "$strict_consumer" >/dev/null; then
		fail "ordinary producer uses the stale-parent maintenance escape: $strict_consumer"
	fi
done

# Exercise recreation against real Git histories while stubbing only unrelated
# source materialization, ABI, and path-ownership policy. The old replay has a
# different subject and stable patch ID from the checked-in replacement, which
# is the normal stale-series case that --recreate must repair.
PREPARE_FIXTURE="$TEST_ROOT/prepare-fixture"
PREPARE_TEMPLATE_SOURCE="$TEST_ROOT/prepare-template-source"
PREPARE_PATCH_DIR="$PREPARE_FIXTURE/kernel/mainline-6.18/patches/series"
PREPARE_METADATA="$PREPARE_FIXTURE/kernel/mainline-6.18/patches/metadata.env"
PREPARE_SCRIPT="$PREPARE_FIXTURE/kernel/mainline-6.18/scripts/prepare-source.sh"
PREPARE_GIT_BIN="$PREPARE_FIXTURE/bin"
mkdir -p \
	"$PREPARE_PATCH_DIR" \
	"$PREPARE_FIXTURE/kernel/mainline-6.18/scripts" \
	"$PREPARE_FIXTURE/kernel/common/abi" \
	"$PREPARE_FIXTURE/kernel/common/tooling" \
	"$PREPARE_GIT_BIN"
cp "$PREPARE_SOURCE" "$PREPARE_SCRIPT"
cp "$LINE_ROOT/scripts/select-slice.sh" \
	"$PREPARE_FIXTURE/kernel/mainline-6.18/scripts/select-slice.sh"
for stub in \
	"$PREPARE_FIXTURE/kernel/mainline-6.18/scripts/materialize-git.sh" \
	"$PREPARE_FIXTURE/kernel/common/abi/sync.sh" \
	"$PREPARE_FIXTURE/kernel/common/tooling/verify-series-policy.py"; do
	{
		printf '%s\n' '#!/bin/sh'
		printf '%s\n' 'exit 0'
	} > "$stub"
	chmod +x "$stub"
done
{
	printf '%s\n' '#!/bin/sh'
	printf '%s\n' 'set -eu'
	printf '%s\n' ': "${UML_KERNEL_SLICE_FIXTURE_SOURCE:?}"'
	printf '%s\n' 'printf "%s\n" "$UML_KERNEL_SLICE_FIXTURE_SOURCE"'
} > "$PREPARE_FIXTURE/kernel/mainline-6.18/scripts/source-location.sh"
chmod +x \
	"$PREPARE_FIXTURE/kernel/mainline-6.18/scripts/source-location.sh"

git init -q "$PREPARE_TEMPLATE_SOURCE"
git -C "$PREPARE_TEMPLATE_SOURCE" config user.name 'kernel slice fixture'
git -C "$PREPARE_TEMPLATE_SOURCE" config user.email \
	'kernel-slice-fixture.invalid'
printf '%s\n' upstream > "$PREPARE_TEMPLATE_SOURCE/base.txt"
git -C "$PREPARE_TEMPLATE_SOURCE" add base.txt
git -C "$PREPARE_TEMPLATE_SOURCE" commit -qm 'fixture upstream'
git -C "$PREPARE_TEMPLATE_SOURCE" branch -M upstream-fixture
PREPARE_UPSTREAM_COMMIT=$(git -C "$PREPARE_TEMPLATE_SOURCE" rev-parse HEAD)
PREPARE_UPSTREAM_TREE=$(git -C "$PREPARE_TEMPLATE_SOURCE" \
	rev-parse 'HEAD^{tree}')
git -C "$PREPARE_TEMPLATE_SOURCE" switch -q -c canonical-fixture
printf '%s\n' canonical > "$PREPARE_TEMPLATE_SOURCE/canonical.txt"
git -C "$PREPARE_TEMPLATE_SOURCE" add canonical.txt
git -C "$PREPARE_TEMPLATE_SOURCE" commit -qm 'fixture current topic'
PREPARE_CANONICAL_COMMIT=$(git -C "$PREPARE_TEMPLATE_SOURCE" rev-parse HEAD)
PREPARE_CANONICAL_TREE=$(git -C "$PREPARE_TEMPLATE_SOURCE" \
	rev-parse 'HEAD^{tree}')
git -C "$PREPARE_TEMPLATE_SOURCE" format-patch --no-signature -1 --stdout \
	"$PREPARE_CANONICAL_COMMIT" > "$PREPARE_PATCH_DIR/0001-current.patch"
PREPARE_PATCH_ID=$(git patch-id --stable \
	< "$PREPARE_PATCH_DIR/0001-current.patch" | awk 'NF { print $1; exit }')
PREPARE_PATCH_DIGEST=$(shasum -a 256 \
	"$PREPARE_PATCH_DIR/0001-current.patch" | awk '{ print $1 }')
{
	printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later'
	printf 'order\tfilename\tstable_patch_id\tsha256\tsubject\n'
	printf '1\t0001-current.patch\t%s\t%s\tfixture current topic\n' \
		"$PREPARE_PATCH_ID" "$PREPARE_PATCH_DIGEST"
} > "$PREPARE_PATCH_DIR/manifest.tsv"
git -C "$PREPARE_TEMPLATE_SOURCE" switch -q upstream-fixture
git -C "$PREPARE_TEMPLATE_SOURCE" switch -q -c managed-fixture
printf '%s\n' stale > "$PREPARE_TEMPLATE_SOURCE/stale.txt"
git -C "$PREPARE_TEMPLATE_SOURCE" add stale.txt
git -C "$PREPARE_TEMPLATE_SOURCE" commit -qm 'fixture stale topic'
PREPARE_STALE_TIP=$(git -C "$PREPARE_TEMPLATE_SOURCE" rev-parse HEAD)
PREPARE_STALE_PATCH_ID=$(git -C "$PREPARE_TEMPLATE_SOURCE" \
	show --pretty=format: "$PREPARE_STALE_TIP" | git patch-id --stable | \
	awk 'NF { print $1; exit }')
[ "$PREPARE_STALE_PATCH_ID" != "$PREPARE_PATCH_ID" ] ||
	fail "stale replay fixture does not have a distinct patch ID"
[ "$(git -C "$PREPARE_TEMPLATE_SOURCE" show -s --format='%s' \
	"$PREPARE_STALE_TIP")" = 'fixture stale topic' ] ||
	fail "stale replay fixture does not have its stale subject"

write_prepare_metadata()
{
	prepare_final_tree=$1
	printf '%s\n' \
		'UML_KERNEL_SERIES_SCHEMA=3' \
		"UML_KERNEL_RELEASE_VERSION='$COMMON_RELEASE_VERSION'" \
		"UML_KERNEL_UPSTREAM_REPOSITORY='$COMMON_UPSTREAM_REPOSITORY'" \
		"UML_KERNEL_UPSTREAM_BRANCH='$COMMON_UPSTREAM_BRANCH'" \
		"UML_KERNEL_UPSTREAM_COMMIT='$PREPARE_UPSTREAM_COMMIT'" \
		"UML_KERNEL_UPSTREAM_TREE='$PREPARE_UPSTREAM_TREE'" \
		"UML_KERNEL_BASE_REF='refs/heads/upstream-fixture'" \
		"UML_KERNEL_PATCH_BRANCH='managed-fixture'" \
		"UML_KERNEL_FINAL_TREE='$prepare_final_tree'" \
		'UML_KERNEL_PATCH_COUNT=1' \
		"UML_KERNEL_ABI_VERSION=$COMMON_ABI_VERSION" \
		"UML_KERNEL_ABI_FINGERPRINT='$COMMON_ABI_FINGERPRINT'" \
		> "$PREPARE_METADATA"
}

initialize_prepare_source()
{
	prepare_source=$1
	git clone -q "$PREPARE_TEMPLATE_SOURCE" "$prepare_source"
	git -C "$prepare_source" remote remove origin
	git -C "$prepare_source" branch -f upstream-fixture \
		"$PREPARE_UPSTREAM_COMMIT"
	git -C "$prepare_source" switch -q managed-fixture
	git -C "$prepare_source" config uml.source.releaseVersion \
		"$COMMON_RELEASE_VERSION"
	git -C "$prepare_source" config uml.source.upstreamRepository \
		"$COMMON_UPSTREAM_REPOSITORY"
	git -C "$prepare_source" config uml.source.upstreamBranch \
		"$COMMON_UPSTREAM_BRANCH"
	git -C "$prepare_source" config uml.source.upstreamCommit \
		"$PREPARE_UPSTREAM_COMMIT"
	git -C "$prepare_source" config uml.source.upstreamTree \
		"$PREPARE_UPSTREAM_TREE"
	git -C "$prepare_source" config uml.source.baseRef \
		'refs/heads/upstream-fixture'
}

require_no_replacement_branch()
{
	prepare_source=$1
	replacement_refs=$(git -C "$prepare_source" for-each-ref \
		--format='%(refname)' 'refs/heads/managed-fixture.recreate-*')
	[ -z "$replacement_refs" ] ||
		fail "failed recreation retained temporary branch $replacement_refs"
}

require_prepare_backup()
{
	prepare_source=$1
	[ "$(git -C "$prepare_source" rev-parse \
		"refs/uml/recreate-backups/arm64/$PREPARE_STALE_TIP")" = \
		"$PREPARE_STALE_TIP" ] ||
		fail "recreate failure lost the pre-recreate backup tip"
}

write_prepare_metadata "$PREPARE_CANONICAL_TREE"
PREPARE_SUCCESS_SOURCE="$TEST_ROOT/prepare-success-source"
initialize_prepare_source "$PREPARE_SUCCESS_SOURCE"
UML_KERNEL_SLICE_FIXTURE_SOURCE=$PREPARE_SUCCESS_SOURCE \
	"$PREPARE_SCRIPT" --slice arm64 --recreate --no-fetch \
	>"$TEST_ROOT/prepare-success.out" 2>"$TEST_ROOT/prepare-success.err" || {
	sed -n '1,80p' "$TEST_ROOT/prepare-success.err" >&2
	fail "recreate rejected a stale subject and patch ID"
}
PREPARE_SUCCESS_TIP=$(git -C "$PREPARE_SUCCESS_SOURCE" \
	rev-parse refs/heads/managed-fixture)
[ "$PREPARE_SUCCESS_TIP" != "$PREPARE_STALE_TIP" ] &&
	[ "$(git -C "$PREPARE_SUCCESS_SOURCE" \
	rev-parse 'refs/heads/managed-fixture^{tree}')" = \
	"$PREPARE_CANONICAL_TREE" ] ||
	fail "recreate did not install the current canonical replay"
[ "$(git -C "$PREPARE_SUCCESS_SOURCE" rev-parse \
	"refs/uml/recreate-backups/arm64/$PREPARE_STALE_TIP")" = \
	"$PREPARE_STALE_TIP" ] ||
	fail "successful recreate did not preserve the stale tip"
[ "$(git -C "$PREPARE_SUCCESS_SOURCE" branch --show-current)" = \
	managed-fixture ] ||
	fail "successful recreate did not restore the managed branch checkout"
require_no_replacement_branch "$PREPARE_SUCCESS_SOURCE"

# A failed git-am and a candidate that fails final-tree verification both leave
# the target ref and checkout unchanged and remove the temporary candidate.
REAL_GIT=$(command -v git)
{
	printf '%s\n' '#!/bin/sh'
	printf '%s\n' 'set -eu'
	printf '%s\n' ': "${UML_KERNEL_SLICE_FIXTURE_REAL_GIT:?}"'
	printf '%s\n' 'case "${UML_KERNEL_SLICE_FIXTURE_GIT_MODE:-}" in'
	printf '%s\n' 'apply-fail)'
	printf '%s\n' 'case " $* " in'
	printf '%s\n' '*" am --3way --no-gpg-sign "*) exit 97 ;;'
	printf '%s\n' 'esac'
	printf '%s\n' ';;'
	printf '%s\n' 'cas)'
	printf '%s\n' 'previous='
	printf '%s\n' 'inject=0'
	printf '%s\n' 'for argument do'
	printf '%s\n' 'if [ "$previous" = update-ref ] &&'
	printf '%s\n' '   [ "$argument" = "refs/heads/$UML_KERNEL_SLICE_FIXTURE_TARGET" ]; then'
	printf '%s\n' 'inject=1'
	printf '%s\n' 'fi'
	printf '%s\n' 'previous=$argument'
	printf '%s\n' 'done'
	printf '%s\n' 'if [ "$inject" -eq 1 ] &&'
	printf '%s\n' '   [ ! -e "$UML_KERNEL_SLICE_FIXTURE_CAS_MARKER" ]; then'
	printf '%s\n' '"$UML_KERNEL_SLICE_FIXTURE_REAL_GIT" -C "$UML_KERNEL_SLICE_FIXTURE_SOURCE" update-ref "refs/heads/$UML_KERNEL_SLICE_FIXTURE_TARGET" "$UML_KERNEL_SLICE_FIXTURE_CONCURRENT_TIP" "$UML_KERNEL_SLICE_FIXTURE_OLD_TIP"'
	printf '%s\n' ': > "$UML_KERNEL_SLICE_FIXTURE_CAS_MARKER"'
	printf '%s\n' 'fi'
	printf '%s\n' ';;'
	printf '%s\n' 'esac'
	printf '%s\n' 'exec "$UML_KERNEL_SLICE_FIXTURE_REAL_GIT" "$@"'
} > "$PREPARE_GIT_BIN/git"
chmod +x "$PREPARE_GIT_BIN/git"

PREPARE_APPLY_FAIL_SOURCE="$TEST_ROOT/prepare-apply-fail-source"
initialize_prepare_source "$PREPARE_APPLY_FAIL_SOURCE"
if PATH="$PREPARE_GIT_BIN:$PATH" \
	UML_KERNEL_SLICE_FIXTURE_REAL_GIT=$REAL_GIT \
	UML_KERNEL_SLICE_FIXTURE_GIT_MODE=apply-fail \
	UML_KERNEL_SLICE_FIXTURE_SOURCE=$PREPARE_APPLY_FAIL_SOURCE \
	"$PREPARE_SCRIPT" --slice arm64 --recreate --no-fetch \
	>"$TEST_ROOT/prepare-apply-fail.out" \
	2>"$TEST_ROOT/prepare-apply-fail.err"; then
	fail "recreate accepted an injected current-patch application failure"
fi
[ "$(git -C "$PREPARE_APPLY_FAIL_SOURCE" \
	rev-parse refs/heads/managed-fixture)" = "$PREPARE_STALE_TIP" ] &&
	[ "$(git -C "$PREPARE_APPLY_FAIL_SOURCE" branch --show-current)" = \
	managed-fixture ] ||
	fail "patch application failure changed the managed target"
require_prepare_backup "$PREPARE_APPLY_FAIL_SOURCE"
require_no_replacement_branch "$PREPARE_APPLY_FAIL_SOURCE"

PREPARE_VERIFY_FAIL_SOURCE="$TEST_ROOT/prepare-verify-fail-source"
initialize_prepare_source "$PREPARE_VERIFY_FAIL_SOURCE"
write_prepare_metadata "$STALE_PARENT_TREE"
if UML_KERNEL_SLICE_FIXTURE_SOURCE=$PREPARE_VERIFY_FAIL_SOURCE \
	"$PREPARE_SCRIPT" --slice arm64 --recreate --no-fetch \
	>"$TEST_ROOT/prepare-verify-fail.out" \
	2>"$TEST_ROOT/prepare-verify-fail.err"; then
	fail "recreate accepted a candidate with the wrong final tree"
fi
[ "$(git -C "$PREPARE_VERIFY_FAIL_SOURCE" \
	rev-parse refs/heads/managed-fixture)" = "$PREPARE_STALE_TIP" ] &&
	[ "$(git -C "$PREPARE_VERIFY_FAIL_SOURCE" branch --show-current)" = \
	managed-fixture ] ||
	fail "candidate verification failure changed the managed target"
require_prepare_backup "$PREPARE_VERIFY_FAIL_SOURCE"
require_no_replacement_branch "$PREPARE_VERIFY_FAIL_SOURCE"
write_prepare_metadata "$PREPARE_CANONICAL_TREE"

# Advance the target ref from inside the Git wrapper immediately before the
# script's compare-and-swap. The stale-tip backup must survive, the concurrent
# tip must win, and cleanup must switch to that tip and remove the candidate.
PREPARE_CAS_SOURCE="$TEST_ROOT/prepare-cas-source"
PREPARE_CAS_MARKER="$TEST_ROOT/prepare-cas-injected"
initialize_prepare_source "$PREPARE_CAS_SOURCE"
git -C "$PREPARE_CAS_SOURCE" switch -q -c concurrent-fixture \
	managed-fixture
printf '%s\n' concurrent > "$PREPARE_CAS_SOURCE/concurrent.txt"
git -C "$PREPARE_CAS_SOURCE" add concurrent.txt
git -C "$PREPARE_CAS_SOURCE" commit -qm 'fixture concurrent target update'
PREPARE_CONCURRENT_TIP=$(git -C "$PREPARE_CAS_SOURCE" rev-parse HEAD)
git -C "$PREPARE_CAS_SOURCE" switch -q managed-fixture
if PATH="$PREPARE_GIT_BIN:$PATH" \
	UML_KERNEL_SLICE_FIXTURE_REAL_GIT=$REAL_GIT \
	UML_KERNEL_SLICE_FIXTURE_GIT_MODE=cas \
	UML_KERNEL_SLICE_FIXTURE_SOURCE=$PREPARE_CAS_SOURCE \
	UML_KERNEL_SLICE_FIXTURE_TARGET=managed-fixture \
	UML_KERNEL_SLICE_FIXTURE_CONCURRENT_TIP=$PREPARE_CONCURRENT_TIP \
	UML_KERNEL_SLICE_FIXTURE_OLD_TIP=$PREPARE_STALE_TIP \
	UML_KERNEL_SLICE_FIXTURE_CAS_MARKER=$PREPARE_CAS_MARKER \
	"$PREPARE_SCRIPT" --slice arm64 --recreate --no-fetch \
	>"$TEST_ROOT/prepare-cas.out" 2>"$TEST_ROOT/prepare-cas.err"; then
	fail "recreate overwrote a concurrently advanced target ref"
fi
[ -e "$PREPARE_CAS_MARKER" ] ||
	fail "CAS fixture did not inject its concurrent ref update"
[ "$(git -C "$PREPARE_CAS_SOURCE" \
	rev-parse refs/heads/managed-fixture)" = "$PREPARE_CONCURRENT_TIP" ] ||
	fail "CAS failure overwrote the concurrent target tip"
[ "$(git -C "$PREPARE_CAS_SOURCE" rev-parse \
	"refs/uml/recreate-backups/arm64/$PREPARE_STALE_TIP")" = \
	"$PREPARE_STALE_TIP" ] ||
	fail "CAS failure lost the pre-recreate backup tip"
[ "$(git -C "$PREPARE_CAS_SOURCE" branch --show-current)" = \
	managed-fixture ] ||
	fail "CAS failure did not restore the concurrently updated target checkout"
require_no_replacement_branch "$PREPARE_CAS_SOURCE"

# The publisher independently fails closed on stale parent metadata before it
# reads any build output. This is a small isolated fixture, not an artifact
# publication campaign.
PUBLISH_FIXTURE="$TEST_ROOT/publish-fixture"
mkdir -p \
	"$PUBLISH_FIXTURE/kernel/common/artifacts" \
	"$PUBLISH_FIXTURE/kernel/mainline-6.18/build" \
	"$PUBLISH_FIXTURE/kernel/mainline-6.18/patches" \
	"$PUBLISH_FIXTURE/kernel/mainline-6.18/slices/x86_64/patches" \
	"$PUBLISH_FIXTURE/source" \
	"$PUBLISH_FIXTURE/build" \
	"$PUBLISH_FIXTURE/output"
cp "$KERNEL_ARTIFACT_MODULE" \
	"$PUBLISH_FIXTURE/kernel/common/artifacts/kernel_artifact.py"
cp "$KERNEL_PUBLISHER" \
	"$PUBLISH_FIXTURE/kernel/mainline-6.18/build/publish-artifact.py"
cp "$COMMON_METADATA" \
	"$PUBLISH_FIXTURE/kernel/mainline-6.18/patches/metadata.env"
cp "$SELECTOR_FIXTURE/slices/x86_64/patches/metadata.env" \
	"$PUBLISH_FIXTURE/kernel/mainline-6.18/slices/x86_64/patches/metadata.env"
if python3 -B \
	"$PUBLISH_FIXTURE/kernel/mainline-6.18/build/publish-artifact.py" \
	--source "$PUBLISH_FIXTURE/source" \
	--build "$PUBLISH_FIXTURE/build" \
	--page-size 4k --slice x86_64 \
	--output-root "$PUBLISH_FIXTURE/output" \
	--release-id stale-parent-fixture \
	>"$TEST_ROOT/publisher.out" 2>"$TEST_ROOT/publisher.err"; then
	fail "x86_64 publisher accepted stale parent metadata"
fi
grep -F 'x86_64 patch parent tree differs from the common final tree' \
	"$TEST_ROOT/publisher.err" >/dev/null ||
	fail "x86_64 publisher stale-parent check failed for the wrong reason"

# Rebase maintenance may see metadata already updated to the new common tree.
# It must still derive the overlay's old base from history and reject an
# arbitrary old parent instead of treating pre-updated metadata as authority.
REBASE_FIXTURE="$TEST_ROOT/rebase-fixture"
REBASE_SOURCE="$TEST_ROOT/rebase-source"
mkdir -p \
	"$REBASE_FIXTURE/kernel/common/tooling" \
	"$REBASE_FIXTURE/kernel/mainline-6.18/scripts" \
	"$REBASE_FIXTURE/kernel/mainline-6.18/patches/series" \
	"$REBASE_FIXTURE/kernel/mainline-6.18/slices/x86_64/patches"
cp "$LINE_ROOT/scripts/rebase-patches.sh" \
	"$REBASE_FIXTURE/kernel/mainline-6.18/scripts/rebase-patches.sh"
cp "$LINE_ROOT/scripts/select-slice.sh" \
	"$REBASE_FIXTURE/kernel/mainline-6.18/scripts/select-slice.sh"
cp "$SERIES_POLICY" \
	"$REBASE_FIXTURE/kernel/common/tooling/verify-series-policy.py"
git init -q "$REBASE_SOURCE"
git -C "$REBASE_SOURCE" config user.name 'kernel slice fixture'
git -C "$REBASE_SOURCE" config user.email 'kernel-slice-fixture.invalid'
printf '%s\n' upstream > "$REBASE_SOURCE/upstream.txt"
git -C "$REBASE_SOURCE" add upstream.txt
git -C "$REBASE_SOURCE" commit -qm 'fixture upstream'
git -C "$REBASE_SOURCE" branch -M upstream-fixture
UPSTREAM_COMMIT=$(git -C "$REBASE_SOURCE" rev-parse HEAD)
UPSTREAM_TREE=$(git -C "$REBASE_SOURCE" rev-parse HEAD^{tree})
git -C "$REBASE_SOURCE" switch -q -c common-fixture
mkdir -p "$REBASE_SOURCE/arch/arm64/um"
printf '%s\n' common-one > \
	"$REBASE_SOURCE/arch/arm64/um/common-one.txt"
git -C "$REBASE_SOURCE" add arch/arm64/um/common-one.txt
git -C "$REBASE_SOURCE" commit -qm 'fixture common one'
printf '%s\n' common-two > \
	"$REBASE_SOURCE/arch/arm64/um/common-two.txt"
git -C "$REBASE_SOURCE" add arch/arm64/um/common-two.txt
git -C "$REBASE_SOURCE" commit -qm 'fixture common two'
COMMON_FIXTURE_COMMIT=$(git -C "$REBASE_SOURCE" rev-parse HEAD)
COMMON_FIXTURE_TREE=$(git -C "$REBASE_SOURCE" rev-parse HEAD^{tree})
git -C "$REBASE_SOURCE" format-patch --numbered --no-signature \
	--output-directory \
	"$REBASE_FIXTURE/kernel/mainline-6.18/patches/series" \
	"$UPSTREAM_COMMIT..$COMMON_FIXTURE_COMMIT" >/dev/null
{
	printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later'
	printf 'order\tfilename\tstable_patch_id\tsha256\tsubject\n'
	order=1
	find "$REBASE_FIXTURE/kernel/mainline-6.18/patches/series" \
		-maxdepth 1 -type f -name '*.patch' -print | LC_ALL=C sort |
	while IFS= read -r patch; do
		filename=$(basename -- "$patch")
		patch_id=$(git patch-id --stable < "$patch" |
			awk 'NF { print $1; exit }')
		digest=$(shasum -a 256 "$patch" | awk '{ print $1 }')
		subject=$(git mailinfo /dev/null /dev/null < "$patch" |
			sed -n 's/^Subject: //p')
		printf '%s\t%s\t%s\t%s\t%s\n' \
			"$order" "$filename" "$patch_id" "$digest" "$subject"
		order=$((order + 1))
	done
} > "$REBASE_FIXTURE/kernel/mainline-6.18/patches/series/manifest.tsv"
{
	printf 'pattern\towner\tnext_base_disposition\trationale\n'
	printf 'arch/arm64/um/*\tarm64\tcarry-locally\tfixture owned path\n'
} > "$REBASE_FIXTURE/kernel/mainline-6.18/patches/path-ownership.tsv"
printf 'path\towner\tdecision\tboundary\tnext_action\trationale\n' \
	> "$REBASE_FIXTURE/kernel/mainline-6.18/patches/extraction-decisions.tsv"
printf 'path\towner\tdisposition\torigin\tnext_policy\trationale\n' \
	> "$REBASE_FIXTURE/kernel/mainline-6.18/patches/outside-port-justification.tsv"
git -C "$REBASE_SOURCE" switch -q -c arbitrary-fixture "$UPSTREAM_COMMIT"
printf '%s\n' arbitrary > "$REBASE_SOURCE/arbitrary.txt"
git -C "$REBASE_SOURCE" add arbitrary.txt
git -C "$REBASE_SOURCE" commit -qm 'fixture arbitrary parent'
ARBITRARY_FIXTURE_TREE=$(git -C "$REBASE_SOURCE" rev-parse HEAD^{tree})
[ "$ARBITRARY_FIXTURE_TREE" != "$COMMON_FIXTURE_TREE" ] ||
	fail "arbitrary-parent fixture did not create a distinct tree"
git -C "$REBASE_SOURCE" switch -q -c slice-fixture
printf '%s\n' overlay > "$REBASE_SOURCE/overlay.txt"
git -C "$REBASE_SOURCE" add overlay.txt
git -C "$REBASE_SOURCE" commit -qm 'fixture overlay'
SLICE_FIXTURE_TREE=$(git -C "$REBASE_SOURCE" rev-parse HEAD^{tree})
printf '%s\n' \
	'UML_KERNEL_SERIES_SCHEMA=3' \
	"UML_KERNEL_RELEASE_VERSION='$COMMON_RELEASE_VERSION'" \
	"UML_KERNEL_UPSTREAM_REPOSITORY='$COMMON_UPSTREAM_REPOSITORY'" \
	"UML_KERNEL_UPSTREAM_BRANCH='$COMMON_UPSTREAM_BRANCH'" \
	"UML_KERNEL_UPSTREAM_COMMIT='$UPSTREAM_COMMIT'" \
	"UML_KERNEL_UPSTREAM_TREE='$UPSTREAM_TREE'" \
	"UML_KERNEL_BASE_REF='refs/heads/upstream-fixture'" \
	"UML_KERNEL_PATCH_BRANCH='common-fixture'" \
	"UML_KERNEL_FINAL_TREE='$COMMON_FIXTURE_TREE'" \
	'UML_KERNEL_PATCH_COUNT=2' \
	"UML_KERNEL_ABI_VERSION=$COMMON_ABI_VERSION" \
	"UML_KERNEL_ABI_FINGERPRINT='$COMMON_ABI_FINGERPRINT'" \
	> "$REBASE_FIXTURE/kernel/mainline-6.18/patches/metadata.env"
write_rebase_slice_metadata()
{
	rebase_slice_branch=$1
	rebase_slice_tree=$2
	printf '%s\n' \
		'UML_KERNEL_SERIES_SCHEMA=3' \
		"UML_KERNEL_RELEASE_VERSION='$COMMON_RELEASE_VERSION'" \
		"UML_KERNEL_UPSTREAM_REPOSITORY='$COMMON_UPSTREAM_REPOSITORY'" \
		"UML_KERNEL_UPSTREAM_BRANCH='$COMMON_UPSTREAM_BRANCH'" \
		"UML_KERNEL_UPSTREAM_COMMIT='$UPSTREAM_COMMIT'" \
		"UML_KERNEL_UPSTREAM_TREE='$UPSTREAM_TREE'" \
		"UML_KERNEL_BASE_REF='refs/heads/upstream-fixture'" \
		"UML_KERNEL_PATCH_BRANCH='$rebase_slice_branch'" \
		'UML_KERNEL_SLICE=x86_64' \
		"UML_KERNEL_PARENT_TREE='$COMMON_FIXTURE_TREE'" \
		"UML_KERNEL_FINAL_TREE='$rebase_slice_tree'" \
		'UML_KERNEL_PATCH_COUNT=1' \
		'UML_KERNEL_ARTIFACT_PATCH_COUNT=3' \
		"UML_KERNEL_ABI_VERSION=$COMMON_ABI_VERSION" \
		"UML_KERNEL_ABI_FINGERPRINT='$COMMON_ABI_FINGERPRINT'" \
		> "$REBASE_FIXTURE/kernel/mainline-6.18/slices/x86_64/patches/metadata.env"
}
write_rebase_slice_metadata slice-fixture "$SLICE_FIXTURE_TREE"
printf '%s\n' \
	'#!/bin/sh' \
	"printf '%s\\n' '$REBASE_SOURCE'" \
	> "$REBASE_FIXTURE/kernel/mainline-6.18/scripts/source-location.sh"
chmod +x "$REBASE_FIXTURE/kernel/mainline-6.18/scripts/source-location.sh"
if "$REBASE_FIXTURE/kernel/mainline-6.18/scripts/rebase-patches.sh" \
	--slice x86_64 --source "$REBASE_SOURCE" \
	>"$TEST_ROOT/rebase.out" 2>"$TEST_ROOT/rebase.err"; then
	fail "overlay rebase accepted an arbitrary historical parent"
fi
grep -F \
	'x86_64 overlay parent tree is neither its tracked parent nor the current common tree' \
	"$TEST_ROOT/rebase.err" >/dev/null || {
	sed -n '1,40p' "$TEST_ROOT/rebase.err" >&2
	fail "arbitrary historical parent failed for the wrong reason"
}

# A one-commit prospective common branch can have the exact same tree as the
# real two-topic common replay. The overlay still must reject that common
# history before it reaches git rebase; accepting tree equivalence would lose
# the common component of its generated provenance.
FAKE_COMMON_COMMIT=$(printf '%s\n' 'fixture squashed common' |
	git -C "$REBASE_SOURCE" commit-tree "$COMMON_FIXTURE_TREE" \
		-p "$UPSTREAM_COMMIT")
[ "$FAKE_COMMON_COMMIT" != "$COMMON_FIXTURE_COMMIT" ] &&
	[ "$(git -C "$REBASE_SOURCE" rev-parse \
	"$FAKE_COMMON_COMMIT^{tree}")" = "$COMMON_FIXTURE_TREE" ] &&
	[ "$(git -C "$REBASE_SOURCE" rev-list --count \
	"$UPSTREAM_COMMIT..$FAKE_COMMON_COMMIT")" -eq 1 ] &&
	[ "$(git -C "$REBASE_SOURCE" rev-list --count \
	"$UPSTREAM_COMMIT..$COMMON_FIXTURE_COMMIT")" -eq 2 ] ||
	fail "squashed common-parent fixture does not have the intended topology"
git -C "$REBASE_SOURCE" update-ref refs/heads/fake-common-fixture \
	"$FAKE_COMMON_COMMIT"
git -C "$REBASE_SOURCE" switch -q -c fake-slice-fixture \
	"$FAKE_COMMON_COMMIT"
printf '%s\n' fake-overlay > "$REBASE_SOURCE/fake-overlay.txt"
git -C "$REBASE_SOURCE" add fake-overlay.txt
git -C "$REBASE_SOURCE" commit -qm 'fixture fake-parent overlay'
FAKE_SLICE_TREE=$(git -C "$REBASE_SOURCE" rev-parse HEAD^{tree})
write_rebase_slice_metadata fake-slice-fixture "$FAKE_SLICE_TREE"
git -C "$REBASE_SOURCE" update-ref refs/heads/common-fixture \
	"$FAKE_COMMON_COMMIT" "$COMMON_FIXTURE_COMMIT"
REBASE_GIT_BIN="$TEST_ROOT/rebase-git-bin"
REBASE_REACHED_MARKER="$TEST_ROOT/rebase-reached"
mkdir -p "$REBASE_GIT_BIN"
{
	printf '%s\n' '#!/bin/sh'
	printf '%s\n' 'set -eu'
	printf '%s\n' ': "${UML_KERNEL_SLICE_FIXTURE_REAL_GIT:?}"'
	printf '%s\n' 'for argument do'
	printf '%s\n' 'if [ "$argument" = rebase ]; then'
	printf '%s\n' ': > "$UML_KERNEL_SLICE_FIXTURE_REBASE_MARKER"'
	printf '%s\n' 'exit 86'
	printf '%s\n' 'fi'
	printf '%s\n' 'done'
	printf '%s\n' 'exec "$UML_KERNEL_SLICE_FIXTURE_REAL_GIT" "$@"'
} > "$REBASE_GIT_BIN/git"
chmod +x "$REBASE_GIT_BIN/git"
if PATH="$REBASE_GIT_BIN:$PATH" \
	UML_KERNEL_SLICE_FIXTURE_REAL_GIT=$REAL_GIT \
	UML_KERNEL_SLICE_FIXTURE_REBASE_MARKER=$REBASE_REACHED_MARKER \
	"$REBASE_FIXTURE/kernel/mainline-6.18/scripts/rebase-patches.sh" \
	--slice x86_64 --source "$REBASE_SOURCE" \
	>"$TEST_ROOT/rebase-fake.out" 2>"$TEST_ROOT/rebase-fake.err"; then
	fail "overlay rebase accepted a squashed same-tree common parent"
fi
[ ! -e "$REBASE_REACHED_MARKER" ] ||
	fail "same-tree fake common parent reached git rebase"
grep -F 'history has 1 commits; expected 2' \
	"$TEST_ROOT/rebase-fake.err" >/dev/null || {
	sed -n '1,40p' "$TEST_ROOT/rebase-fake.err" >&2
	fail "same-tree fake common parent failed for the wrong reason"
}
git -C "$REBASE_SOURCE" update-ref refs/heads/common-fixture \
	"$COMMON_FIXTURE_COMMIT" "$FAKE_COMMON_COMMIT"

# Every producer defaults to the normal arm64 slice, exposes the same selector,
# and rejects unknown slice identities before consulting a managed checkout.
for relative in \
	build/build-kernel.sh \
	scripts/prepare-source.sh \
	scripts/rebase-patches.sh \
	scripts/audit-delta.sh; do
	producer="$LINE_ROOT/$relative"
	require_file "$producer"
	"$producer" --help >"$TEST_ROOT/help.out"
	grep -F -- '--slice' "$TEST_ROOT/help.out" >/dev/null &&
		grep -F 'arm64' "$TEST_ROOT/help.out" >/dev/null &&
		grep -F 'x86_64' "$TEST_ROOT/help.out" >/dev/null ||
		fail "$relative does not document the shared slice selector"
	grep -E '^SLICE=arm64$' "$producer" >/dev/null ||
		fail "$relative does not default to the arm64 slice"
	if "$producer" --slice invalid \
		>"$TEST_ROOT/invalid.out" 2>"$TEST_ROOT/invalid.err"; then
		fail "$relative accepted an unknown kernel slice"
	fi
	grep -F 'kernel slice must be arm64 or x86_64' \
		"$TEST_ROOT/invalid.err" >/dev/null ||
		fail "$relative rejected an unknown slice for the wrong reason"
done

# Exercise the page-profile guard with an isolated selector fixture so this
# policy test never needs a configured Linux checkout or compiler toolchain.
BUILD_FIXTURE="$TEST_ROOT/build-fixture"
mkdir -p \
	"$BUILD_FIXTURE/kernel/common" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/build/toolchain" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/scripts" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/patches" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/slices/x86_64/patches"
cp "$LINE_ROOT/build/build-kernel.sh" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/build/build-kernel.sh"
cp "$LINE_ROOT/build/toolchain/darwin.env" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/build/toolchain/darwin.env"
cp "$LINE_ROOT/scripts/select-slice.sh" \
	"$BUILD_FIXTURE/kernel/mainline-6.18/scripts/select-slice.sh"
printf '%s\n' \
	'UML_KERNEL_SERIES_SCHEMA=3' \
	"UML_KERNEL_RELEASE_VERSION='$COMMON_RELEASE_VERSION'" \
	"UML_KERNEL_UPSTREAM_REPOSITORY='$COMMON_UPSTREAM_REPOSITORY'" \
	"UML_KERNEL_UPSTREAM_BRANCH='$COMMON_UPSTREAM_BRANCH'" \
	"UML_KERNEL_UPSTREAM_COMMIT='$COMMON_UPSTREAM_COMMIT'" \
	"UML_KERNEL_UPSTREAM_TREE='$COMMON_UPSTREAM_TREE'" \
	"UML_KERNEL_BASE_REF='$COMMON_BASE_REF'" \
	"UML_KERNEL_PATCH_BRANCH='$COMMON_PATCH_BRANCH'" \
	"UML_KERNEL_FINAL_TREE='$COMMON_FINAL_TREE'" \
	"UML_KERNEL_PATCH_COUNT=$COMMON_PATCH_COUNT" \
	"UML_KERNEL_ABI_VERSION=$COMMON_ABI_VERSION" \
	"UML_KERNEL_ABI_FINGERPRINT='$COMMON_ABI_FINGERPRINT'" \
	> "$BUILD_FIXTURE/kernel/mainline-6.18/patches/metadata.env"
printf '%s\n' \
	'UML_KERNEL_SERIES_SCHEMA=3' \
	"UML_KERNEL_RELEASE_VERSION='$COMMON_RELEASE_VERSION'" \
	"UML_KERNEL_UPSTREAM_REPOSITORY='$COMMON_UPSTREAM_REPOSITORY'" \
	"UML_KERNEL_UPSTREAM_BRANCH='$COMMON_UPSTREAM_BRANCH'" \
	"UML_KERNEL_UPSTREAM_COMMIT='$COMMON_UPSTREAM_COMMIT'" \
	"UML_KERNEL_UPSTREAM_TREE='$COMMON_UPSTREAM_TREE'" \
	"UML_KERNEL_BASE_REF='$COMMON_BASE_REF'" \
	"UML_KERNEL_PATCH_BRANCH='$SLICE_PATCH_BRANCH'" \
	"UML_KERNEL_FINAL_TREE='$SLICE_FINAL_TREE'" \
	"UML_KERNEL_PATCH_COUNT=$SLICE_PATCH_COUNT" \
	'UML_KERNEL_SLICE=x86_64' \
	"UML_KERNEL_PARENT_TREE='$COMMON_FINAL_TREE'" \
	"UML_KERNEL_ABI_VERSION=$COMMON_ABI_VERSION" \
	"UML_KERNEL_ABI_FINGERPRINT='$COMMON_ABI_FINGERPRINT'" \
	> "$BUILD_FIXTURE/kernel/mainline-6.18/slices/x86_64/patches/metadata.env"
printf '%s\n' \
	'#!/bin/sh' \
	"printf '%s\\n' '$TEST_ROOT/fake-linux'" \
	> "$BUILD_FIXTURE/kernel/mainline-6.18/scripts/source-location.sh"
chmod +x "$BUILD_FIXTURE/kernel/mainline-6.18/scripts/source-location.sh"
if UML_KERNEL_JOBS=1 \
	"$BUILD_FIXTURE/kernel/mainline-6.18/build/build-kernel.sh" \
		--slice x86_64 --build "$TEST_ROOT/build-x86-16k" \
		--page-size 16k >"$TEST_ROOT/x86-16k.out" \
		2>"$TEST_ROOT/x86-16k.err"; then
	fail "x86_64 kernel slice accepted the 16 KiB page profile"
fi
grep -F 'x86_64 userspace slice supports only the 4k page profile' \
	"$TEST_ROOT/x86-16k.err" >/dev/null ||
	fail "x86_64 16 KiB rejection did not identify the slice constraint"

# Run verify-replay against no-op producer doubles. This checks the complete
# ordered matrix without downloading Linux or spending a full kernel build.
REPLAY_FIXTURE="$TEST_ROOT/replay-fixture"
REPLAY_LOG="$TEST_ROOT/replay.log"
mkdir -p \
	"$REPLAY_FIXTURE/kernel/mainline-6.18/scripts" \
	"$REPLAY_FIXTURE/kernel/mainline-6.18/build" \
	"$REPLAY_FIXTURE/tests/tooling/kernel" \
	"$TEST_ROOT/replay-work"
cp "$LINE_ROOT/scripts/verify-replay.sh" \
	"$REPLAY_FIXTURE/kernel/mainline-6.18/scripts/verify-replay.sh"
printf '%s\n' \
	'#!/bin/sh' \
	'set -eu' \
	'label=${0##*/}' \
	'printf "%s" "$label" >> "$UML_KERNEL_SLICE_TEST_LOG"' \
	'for argument do' \
	'printf " %s" "$argument" >> "$UML_KERNEL_SLICE_TEST_LOG"' \
	'done' \
	'printf "\\n" >> "$UML_KERNEL_SLICE_TEST_LOG"' \
	> "$TEST_ROOT/producer-double.sh"
chmod +x "$TEST_ROOT/producer-double.sh"
ln -s "$TEST_ROOT/producer-double.sh" \
	"$REPLAY_FIXTURE/kernel/mainline-6.18/scripts/prepare-source.sh"
ln -s "$TEST_ROOT/producer-double.sh" \
	"$REPLAY_FIXTURE/kernel/mainline-6.18/scripts/audit-delta.sh"
ln -s "$TEST_ROOT/producer-double.sh" \
	"$REPLAY_FIXTURE/kernel/mainline-6.18/build/build-kernel.sh"
printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later' \
	> "$REPLAY_FIXTURE/tests/tooling/kernel/test_darwin_init_mm_tlb_policy.py"
printf '%s\n' '# SPDX-License-Identifier: GPL-2.0-or-later' \
	> "$REPLAY_FIXTURE/tests/tooling/kernel/test_darwin_vcpu_carrier_policy.py"
UML_KERNEL_SLICE_TEST_LOG=$REPLAY_LOG
export UML_KERNEL_SLICE_TEST_LOG
"$REPLAY_FIXTURE/kernel/mainline-6.18/scripts/verify-replay.sh" \
	--work-root "$TEST_ROOT/replay-work" --jobs 7 \
	>"$TEST_ROOT/replay.out" 2>"$TEST_ROOT/replay.err" || {
	sed -n '1,80p' "$TEST_ROOT/replay.err" >&2
	fail "verify-replay matrix fixture failed"
}
awk '
NR == 1 { good += ($0 == "prepare-source.sh --slice arm64") }
NR == 2 { good += ($0 == "prepare-source.sh --slice arm64 --verify --no-fetch") }
NR == 3 { good += ($0 == "audit-delta.sh --slice arm64 --tree HEAD") }
NR == 4 { good += ($1 == "build-kernel.sh" && $2 == "--slice" &&
	$3 == "arm64" && $4 == "--build" && $5 ~ /\/build-arm64-16k$/ &&
	$6 == "--page-size" && $7 == "16k" && $8 == "--jobs" && $9 == "7") }
NR == 5 { good += ($1 == "build-kernel.sh" && $2 == "--slice" &&
	$3 == "arm64" && $4 == "--build" && $5 ~ /\/build-arm64-4k$/ &&
	$6 == "--page-size" && $7 == "4k" && $8 == "--jobs" && $9 == "7") }
NR == 6 { good += ($0 == "prepare-source.sh --slice x86_64 --no-fetch") }
NR == 7 { good += ($0 == "prepare-source.sh --slice x86_64 --verify --no-fetch") }
NR == 8 { good += ($0 == "audit-delta.sh --slice x86_64 --tree HEAD") }
NR == 9 { good += ($1 == "build-kernel.sh" && $2 == "--slice" &&
	$3 == "x86_64" && $4 == "--build" && $5 ~ /\/build-x86_64-4k$/ &&
	$6 == "--page-size" && $7 == "4k" && $8 == "--jobs" && $9 == "7") }
END { exit !(NR == 9 && good == 9) }
' "$REPLAY_LOG" || {
	sed -n '1,40p' "$REPLAY_LOG" >&2
	fail "verify-replay does not cover arm64 16k/4k followed by x86_64 4k"
}

printf '%s\n' "kernel common/x86_64 slice producer contract passed"
