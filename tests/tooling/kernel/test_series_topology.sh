#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

REPOSITORY=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIER="$REPOSITORY/kernel/verify-series-policy.py"
WORK_DIRECTORY=$(mktemp -d "${TMPDIR:-/tmp}/uml-series-topology.XXXXXX")

cleanup()
{
	rm -rf -- "$WORK_DIRECTORY"
}
trap cleanup 0 HUP INT TERM

SOURCE="$WORK_DIRECTORY/source"
FIXTURE="$WORK_DIRECTORY/fixture"
mkdir -p "$SOURCE" "$FIXTURE/kernel/patches/series"
git -C "$SOURCE" init -q
git -C "$SOURCE" config user.name 'series topology test'
git -C "$SOURCE" config user.email 'noreply@linux-um-darwin.invalid'
printf '%s\n' base > "$SOURCE/base"
git -C "$SOURCE" add base
git -C "$SOURCE" commit -q -m base
BASE=$(git -C "$SOURCE" rev-parse HEAD)

mkdir -p "$SOURCE/arch/arm64" "$SOURCE/arch/um"
printf '%s\n' profile > "$SOURCE/arch/arm64/Makefile.um"
printf '%s\n' one > "$SOURCE/arch/um/one"
git -C "$SOURCE" add arch/arm64/Makefile.um arch/um/one
git -C "$SOURCE" commit -q -m 'topic one'
COMMIT_ONE=$(git -C "$SOURCE" rev-parse HEAD)
printf '%s\n' two > "$SOURCE/two"
git -C "$SOURCE" add two
git -C "$SOURCE" commit -q -m 'topic two'
COMMIT_TWO=$(git -C "$SOURCE" rev-parse HEAD)
CANONICAL_TIP=$COMMIT_TWO

git -C "$SOURCE" format-patch --subject-prefix='PATCH 1/2' -1 \
	--stdout "$COMMIT_ONE" \
	> "$FIXTURE/kernel/patches/series/0001.patch"
git -C "$SOURCE" format-patch --subject-prefix='PATCH 2/2' -1 \
	--stdout "$COMMIT_TWO" \
	> "$FIXTURE/kernel/patches/series/0002.patch"
printf '%s\n' 'UML_KERNEL_PATCH_COUNT=2' \
	> "$FIXTURE/kernel/patches/metadata.env"
printf '%s\n' 'path	topic_orders	rationale' \
	> "$FIXTURE/kernel/patches/topic-path-overlaps.tsv"

write_manifest()
{
	{
	printf '%s\n' 'order	filename	stable_patch_id	sha256	subject'
	order=1
	for patch in "$FIXTURE"/kernel/patches/series/*.patch; do
		patch_id=$(git patch-id --stable < "$patch" | awk '{print $1}')
		digest=$(shasum -a 256 "$patch" | awk '{print $1}')
		subject=$(git mailinfo "$WORK_DIRECTORY/message" \
			"$WORK_DIRECTORY/body" < "$patch" | sed -n 's/^Subject: //p')
		printf '%s\t%s\t%s\t%s\t%s\n' "$order" \
			"$(basename -- "$patch")" "$patch_id" "$digest" "$subject"
		order=$((order + 1))
	done
	} > "$FIXTURE/kernel/patches/series/manifest.tsv"
}
write_manifest

verify()
{
	"$VERIFIER" --root "$FIXTURE" --source "$SOURCE" --base "$BASE" \
		--tip "$1" topology >/dev/null
}

verify_manifest()
{
	"$VERIFIER" --root "$FIXTURE" manifest >/dev/null
}

reject()
{
	label=$1
	tip=$2
	if verify "$tip" 2>/dev/null; then
		printf '%s\n' "topology verifier accepted $label history" >&2
		exit 1
	fi
}

cp "$FIXTURE/kernel/patches/series/0001.patch" \
	"$WORK_DIRECTORY/canonical-0001.patch"
awk '
	!changed && /^\+one$/ { print $0 " "; changed = 1; next }
	{ print }
	END { if (!changed) exit 2 }
' "$WORK_DIRECTORY/canonical-0001.patch" \
	> "$FIXTURE/kernel/patches/series/0001.patch"
write_manifest
WHITESPACE_ERROR="$WORK_DIRECTORY/added-source-whitespace.err"
if verify_manifest 2>"$WHITESPACE_ERROR"; then
	printf '%s\n' \
		'manifest verifier accepted added-source trailing whitespace' >&2
	exit 1
fi
if ! grep -q 'added source line has trailing whitespace' \
	"$WHITESPACE_ERROR"; then
	printf '%s\n' \
		'manifest verifier rejected the whitespace fixture for the wrong reason' >&2
	sed -n '1,20p' "$WHITESPACE_ERROR" >&2
	exit 1
fi
cp "$WORK_DIRECTORY/canonical-0001.patch" \
	"$FIXTURE/kernel/patches/series/0001.patch"
write_manifest
verify_manifest

awk '
	!changed && /^Subject: \[PATCH 1\/2\] / {
		sub(/\[PATCH 1\/2\]/, "[PATCH 2/2]")
		changed = 1
	}
	{ print }
	END { if (!changed) exit 2 }
' "$WORK_DIRECTORY/canonical-0001.patch" \
	> "$FIXTURE/kernel/patches/series/0001.patch"
write_manifest
WRONG_ORDINAL="$WORK_DIRECTORY/wrong-subject-ordinal.err"
if verify_manifest 2>"$WRONG_ORDINAL"; then
	printf '%s\n' \
		'manifest verifier accepted the wrong patch Subject ordinal' >&2
	exit 1
fi
if ! grep -F -q 'expected [PATCH 1/2]' "$WRONG_ORDINAL"; then
	printf '%s\n' \
		'manifest verifier rejected the Subject ordinal for the wrong reason' >&2
	sed -n '1,20p' "$WRONG_ORDINAL" >&2
	exit 1
fi

awk '
	!changed && /^Subject: \[PATCH 1\/2\] / {
		sub(/\[PATCH 1\/2\]/, "[PATCH 1/3]")
		changed = 1
	}
	{ print }
	END { if (!changed) exit 2 }
' "$WORK_DIRECTORY/canonical-0001.patch" \
	> "$FIXTURE/kernel/patches/series/0001.patch"
write_manifest
WRONG_TOTAL="$WORK_DIRECTORY/wrong-subject-total.err"
if verify_manifest 2>"$WRONG_TOTAL"; then
	printf '%s\n' \
		'manifest verifier accepted the wrong patch Subject total' >&2
	exit 1
fi
if ! grep -F -q 'expected [PATCH 1/2]' "$WRONG_TOTAL"; then
	printf '%s\n' \
		'manifest verifier rejected the Subject total for the wrong reason' >&2
	sed -n '1,20p' "$WRONG_TOTAL" >&2
	exit 1
fi
cp "$WORK_DIRECTORY/canonical-0001.patch" \
	"$FIXTURE/kernel/patches/series/0001.patch"
write_manifest
verify_manifest

verify "$CANONICAL_TIP"

printf '%s\n' 'path	topic_orders	rationale' \
	'arch/um/one	0001,0002	stale fixture overlap' \
	> "$FIXTURE/kernel/patches/topic-path-overlaps.tsv"
if verify_manifest 2>/dev/null; then
	printf '%s\n' 'manifest verifier accepted a stale topic-path overlap' >&2
	exit 1
fi
printf '%s\n' 'path	topic_orders	rationale' \
	> "$FIXTURE/kernel/patches/topic-path-overlaps.tsv"

cp "$FIXTURE/kernel/patches/series/0002.patch" \
	"$WORK_DIRECTORY/canonical-0002.patch"
git -C "$SOURCE" switch -q -c overlap-policy-case "$CANONICAL_TIP"
printf '%s\n' changed > "$SOURCE/arch/um/one"
git -C "$SOURCE" add arch/um/one
git -C "$SOURCE" commit -q -m 'overlap policy topic'
git -C "$SOURCE" format-patch --subject-prefix='PATCH 2/2' -1 --stdout HEAD \
	> "$FIXTURE/kernel/patches/series/0002.patch"
write_manifest
if verify_manifest 2>/dev/null; then
	printf '%s\n' 'manifest verifier accepted an undeclared topic-path overlap' >&2
	exit 1
fi
printf '%s\n' 'path	topic_orders	rationale' \
	'arch/um/one	0001,0003	wrong owner fixture' \
	> "$FIXTURE/kernel/patches/topic-path-overlaps.tsv"
if verify_manifest 2>/dev/null; then
	printf '%s\n' 'manifest verifier accepted the wrong overlap owners' >&2
	exit 1
fi
printf '%s\n' 'path	topic_orders	rationale' \
	'arch/um/one	0001,0002	intentional fixture overlap' \
	> "$FIXTURE/kernel/patches/topic-path-overlaps.tsv"
verify_manifest
cp "$WORK_DIRECTORY/canonical-0002.patch" \
	"$FIXTURE/kernel/patches/series/0002.patch"
printf '%s\n' 'path	topic_orders	rationale' \
	> "$FIXTURE/kernel/patches/topic-path-overlaps.tsv"
write_manifest
verify "$CANONICAL_TIP"

git -C "$SOURCE" switch -q -c makefile-overlap-case "$CANONICAL_TIP"
printf '%s\n' scaled-profile > "$SOURCE/arch/arm64/Makefile.um"
git -C "$SOURCE" add arch/arm64/Makefile.um
git -C "$SOURCE" commit -q -m 'address profile topic'
git -C "$SOURCE" format-patch --subject-prefix='PATCH 2/2' -1 --stdout HEAD \
	> "$FIXTURE/kernel/patches/series/0002.patch"
write_manifest
printf '%s\n' 'path	topic_orders	rationale' \
	'arch/arm64/Makefile.um	0001,0002	intentional profile overlap' \
	> "$FIXTURE/kernel/patches/topic-path-overlaps.tsv"
verify_manifest
printf '%s\n' 'path	topic_orders	rationale' \
	'arch/arm64/Makefile	0001,0002	invalid broad build overlap' \
	> "$FIXTURE/kernel/patches/topic-path-overlaps.tsv"
OUTSIDE_PORT="$WORK_DIRECTORY/outside-port-overlap.err"
if verify_manifest 2>"$OUTSIDE_PORT"; then
	printf '%s\n' 'manifest verifier accepted a build path outside the port' >&2
	exit 1
fi
if ! grep -F -q 'topic overlap is outside the port-owned paths' \
	"$OUTSIDE_PORT"; then
	printf '%s\n' \
		'manifest verifier rejected the outside-port path for the wrong reason' >&2
	sed -n '1,20p' "$OUTSIDE_PORT" >&2
	exit 1
fi
cp "$WORK_DIRECTORY/canonical-0002.patch" \
	"$FIXTURE/kernel/patches/series/0002.patch"
printf '%s\n' 'path	topic_orders	rationale' \
	> "$FIXTURE/kernel/patches/topic-path-overlaps.tsv"
write_manifest
verify "$CANONICAL_TIP"

cp "$FIXTURE/kernel/patches/series/0001.patch" \
	"$FIXTURE/kernel/patches/series/unlisted.patch"
reject unlisted-patch "$CANONICAL_TIP"
rm -f -- "$FIXTURE/kernel/patches/series/unlisted.patch"
verify "$CANONICAL_TIP"

git -C "$SOURCE" switch -q -c empty-case "$CANONICAL_TIP"
git -C "$SOURCE" commit -q --allow-empty -m 'empty topic'
reject empty HEAD

git -C "$SOURCE" switch -q -c extra-case "$CANONICAL_TIP"
printf '%s\n' extra > "$SOURCE/extra"
git -C "$SOURCE" add extra
git -C "$SOURCE" commit -q -m 'extra topic'
reject extra HEAD

git -C "$SOURCE" switch -q -c reordered-case "$BASE"
git -C "$SOURCE" cherry-pick "$COMMIT_TWO" >/dev/null
git -C "$SOURCE" cherry-pick "$COMMIT_ONE" >/dev/null
reject reordered HEAD

git -C "$SOURCE" switch -q -c merge-side "$BASE"
git -C "$SOURCE" cherry-pick "$COMMIT_ONE" >/dev/null
git -C "$SOURCE" switch -q -c merge-case "$BASE"
git -C "$SOURCE" merge -q --no-ff -m 'merge topic' merge-side
reject merge HEAD

printf '%s\n' "Linux series topology negative tests passed"
