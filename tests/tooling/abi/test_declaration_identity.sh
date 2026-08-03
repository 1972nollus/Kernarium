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

mkdir -p "$WORK_DIRECTORY/kernel" "$WORK_DIRECTORY/darwin/api/include/uml"
cp -R "$REPOSITORY/kernel/abi" "$WORK_DIRECTORY/kernel/abi"
cp "$REPOSITORY/darwin/api/include/uml/emulator.h" \
	"$WORK_DIRECTORY/darwin/api/include/uml/emulator.h"

VERIFY="$REPOSITORY/kernel/abi/verify-declarations.py"
"$PYTHON" "$VERIFY" --root "$WORK_DIRECTORY" >/dev/null

printf '%s\n' '/* Comment-only edits do not alter declaration identity. */' >> \
	"$WORK_DIRECTORY/darwin/api/include/uml/emulator.h"
"$PYTHON" "$VERIFY" --root "$WORK_DIRECTORY" >/dev/null

printf '%s\n' 'int uml_emulator_unrecorded_abi_change(void);' >> \
	"$WORK_DIRECTORY/darwin/api/include/uml/emulator.h"
if "$PYTHON" "$VERIFY" --root "$WORK_DIRECTORY" >/dev/null 2>&1; then
	printf '%s\n' \
		"ABI declaration mutation passed without an identity-history update" >&2
	exit 1
fi

printf '%s\n' "ABI declaration identity negative test passed"
