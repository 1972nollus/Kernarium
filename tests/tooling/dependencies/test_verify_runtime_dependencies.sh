#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIER="$ROOT_DIR/dependencies/verify-runtime-dependencies.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-runtime-dependencies.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

"$VERIFIER" --binary /usr/bin/true >/dev/null

cat >"$TEST_ROOT/library.c" <<'EOF'
int uml_runtime_dependency_fixture(void) { return 0; }
EOF
cat >"$TEST_ROOT/main.c" <<'EOF'
int uml_runtime_dependency_fixture(void);
int main(void) { return uml_runtime_dependency_fixture(); }
EOF
xcrun --sdk macosx clang -arch arm64 -dynamiclib \
	-Wl,-install_name,"$TEST_ROOT/libambient.dylib" \
	"$TEST_ROOT/library.c" -o "$TEST_ROOT/libambient.dylib"
xcrun --sdk macosx clang -arch arm64 "$TEST_ROOT/main.c" \
	"$TEST_ROOT/libambient.dylib" -o "$TEST_ROOT/ambient-client"

if "$VERIFIER" --binary "$TEST_ROOT/ambient-client" \
	>"$TEST_ROOT/rejected.out" 2>"$TEST_ROOT/rejected.err"; then
	printf '%s\n' 'non-system dynamic dependency was accepted' >&2
	exit 1
fi
grep -F 'non-system dynamic dependency' "$TEST_ROOT/rejected.err" >/dev/null || {
	printf '%s\n' 'dynamic-dependency rejection lacked its diagnostic' >&2
	exit 1
}

printf '%s\n' 'runtime dependency closure fixture passed'
