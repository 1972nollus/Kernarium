#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

REPOSITORY=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
PYTHON=${PYTHON:-python3}
WORK_DIRECTORY=$(mktemp -d "${TMPDIR:-/tmp}/uml-abi-identity.XXXXXX")

cleanup()
{
	rm -rf -- "$WORK_DIRECTORY"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$WORK_DIRECTORY/kernel/common" \
	"$WORK_DIRECTORY/darwin/api/include/uml" \
	"$WORK_DIRECTORY/darwin/api"
cp -R "$REPOSITORY/kernel/common/abi" "$WORK_DIRECTORY/kernel/common/abi"
cp -R "$REPOSITORY/darwin/api/abi" "$WORK_DIRECTORY/darwin/api/abi"
cp "$REPOSITORY/darwin/api/include/uml/emulator.h" \
	"$WORK_DIRECTORY/darwin/api/include/uml/emulator.h"
cp "$REPOSITORY/darwin/api/include/uml/abi_identity.h" \
	"$WORK_DIRECTORY/darwin/api/include/uml/abi_identity.h"

VERIFY="$REPOSITORY/kernel/common/abi/verify-declarations.py"
"$PYTHON" "$VERIFY" --root "$WORK_DIRECTORY" >/dev/null

printf '%s\n' '/* Comment-only edits do not alter declaration identity. */' >> \
	"$WORK_DIRECTORY/darwin/api/include/uml/emulator.h"
"$PYTHON" "$VERIFY" --root "$WORK_DIRECTORY" >/dev/null

PUBLIC_BEFORE=$(
	"$PYTHON" "$VERIFY" --root "$WORK_DIRECTORY" --domain public --compute
)
printf '%s\n' 'int darwin_uml_unrecorded_bridge_change(void);' >> \
	"$WORK_DIRECTORY/kernel/common/abi/darwin_uml_host.h"
[ "$("$PYTHON" "$VERIFY" --root "$WORK_DIRECTORY" \
	--domain public --compute)" = "$PUBLIC_BEFORE" ] || {
	printf '%s\n' "kernel-only mutation changed public identity" >&2
	exit 1
}
if "$PYTHON" "$VERIFY" --root "$WORK_DIRECTORY" --domain kernel \
	>/dev/null 2>&1; then
	printf '%s\n' \
		"kernel declaration mutation passed without a kernel history update" >&2
	exit 1
fi
cp "$REPOSITORY/kernel/common/abi/darwin_uml_host.h" \
	"$WORK_DIRECTORY/kernel/common/abi/darwin_uml_host.h"

KERNEL_BEFORE=$(
	"$PYTHON" "$VERIFY" --root "$WORK_DIRECTORY" --domain kernel --compute
)
printf '%s\n' 'int uml_emulator_unrecorded_api_change(void);' >> \
	"$WORK_DIRECTORY/darwin/api/include/uml/emulator.h"
[ "$("$PYTHON" "$VERIFY" --root "$WORK_DIRECTORY" \
	--domain kernel --compute)" = "$KERNEL_BEFORE" ] || {
	printf '%s\n' "public-only mutation changed kernel identity" >&2
	exit 1
}
if "$PYTHON" "$VERIFY" --root "$WORK_DIRECTORY" --domain public \
	>/dev/null 2>&1; then
	printf '%s\n' \
		"public declaration mutation passed without a public history update" >&2
	exit 1
fi

IMPORT_VERIFY="$REPOSITORY/kernel/common/abi/verify-linux-imports.sh"
sed -n -E 's/.*(darwin_uml_[A-Za-z0-9_]+)\(.*/\1/p' \
	"$REPOSITORY"/kernel/common/abi/darwin_uml_*.h |
	LC_ALL=C sort -u > "$WORK_DIRECTORY/all-symbols"
awk -F '\t' 'NR > 1 { print $1 }' \
	"$REPOSITORY/kernel/common/abi/import-policy.tsv" |
	LC_ALL=C sort > "$WORK_DIRECTORY/4k-only-symbols"
comm -23 "$WORK_DIRECTORY/all-symbols" "$WORK_DIRECTORY/4k-only-symbols" \
	> "$WORK_DIRECTORY/16k-symbols"
awk '{ print "                 U " $0 }' "$WORK_DIRECTORY/all-symbols" \
	> "$WORK_DIRECTORY/4k.nm"
awk '{ print "                 U " $0 }' "$WORK_DIRECTORY/16k-symbols" \
	> "$WORK_DIRECTORY/16k.nm"
"$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/4k.nm" --page-size 4k \
	>/dev/null
"$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/16k.nm" --page-size 16k \
	>/dev/null
if "$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/4k.nm" --page-size 16k \
	>/dev/null 2>&1; then
	printf '%s\n' "16 KiB import audit accepted a 4 KiB-only symbol" >&2
	exit 1
fi
sed '1d' "$WORK_DIRECTORY/16k.nm" > "$WORK_DIRECTORY/16k-missing.nm"
if "$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/16k-missing.nm" \
	--page-size 16k >/dev/null 2>&1; then
	printf '%s\n' "16 KiB import audit accepted a missing active symbol" >&2
	exit 1
fi

printf '%s\n' \
	"independent ABI identity and page-specific import negative tests passed"
