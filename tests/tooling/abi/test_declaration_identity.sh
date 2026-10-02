#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

REPOSITORY=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
PYTHON=${PYTHON:-python3}
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-abi-identity.XXXXXX")
TEST_NAME=test_declaration_identity.sh
. "$REPOSITORY/tests/tooling/support/expect.sh"

cleanup()
{
	rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$TEST_ROOT/kernel/common" \
	"$TEST_ROOT/darwin/api/include/uml" \
	"$TEST_ROOT/darwin/api"
cp -R "$REPOSITORY/kernel/common/abi" "$TEST_ROOT/kernel/common/abi"
cp -R "$REPOSITORY/darwin/api/abi" "$TEST_ROOT/darwin/api/abi"
cp "$REPOSITORY/darwin/api/include/uml/emulator.h" \
	"$TEST_ROOT/darwin/api/include/uml/emulator.h"
cp "$REPOSITORY/darwin/api/include/uml/abi_identity.h" \
	"$TEST_ROOT/darwin/api/include/uml/abi_identity.h"

VERIFY="$REPOSITORY/kernel/common/abi/verify-declarations.py"
"$PYTHON" "$VERIFY" --root "$TEST_ROOT" >/dev/null

printf '%s\n' '/* Comment-only edits do not alter declaration identity. */' >> \
	"$TEST_ROOT/darwin/api/include/uml/emulator.h"
"$PYTHON" "$VERIFY" --root "$TEST_ROOT" >/dev/null

PUBLIC_BEFORE=$(
	"$PYTHON" "$VERIFY" --root "$TEST_ROOT" --domain public --compute
)
printf '%s\n' 'int darwin_uml_unrecorded_bridge_change(void);' >> \
	"$TEST_ROOT/kernel/common/abi/darwin_uml_host.h"
[ "$("$PYTHON" "$VERIFY" --root "$TEST_ROOT" \
	--domain public --compute)" = "$PUBLIC_BEFORE" ] ||
	fail "kernel-only mutation changed public identity"
expect_failure "kernel declaration mutation without a kernel identity update" \
	"$PYTHON" "$VERIFY" --root "$TEST_ROOT" --domain kernel
cp "$REPOSITORY/kernel/common/abi/darwin_uml_host.h" \
	"$TEST_ROOT/kernel/common/abi/darwin_uml_host.h"

KERNEL_BEFORE=$(
	"$PYTHON" "$VERIFY" --root "$TEST_ROOT" --domain kernel --compute
)
printf '%s\n' 'int uml_emulator_unrecorded_api_change(void);' >> \
	"$TEST_ROOT/darwin/api/include/uml/emulator.h"
[ "$("$PYTHON" "$VERIFY" --root "$TEST_ROOT" \
	--domain kernel --compute)" = "$KERNEL_BEFORE" ] ||
	fail "public-only mutation changed kernel identity"
expect_failure "public declaration mutation without a public identity update" \
	"$PYTHON" "$VERIFY" --root "$TEST_ROOT" --domain public

# sync.sh writes each identity header from its identity.env and --verify
# rejects a header that no longer matches. Run the copied tool so it resolves
# the fixture tree as its repository root.
SYNC_ROOT="$TEST_ROOT/sync"
mkdir -p "$SYNC_ROOT/kernel/common" "$SYNC_ROOT/darwin/api/include/uml" \
	"$SYNC_ROOT/linux"
cp -R "$REPOSITORY/kernel/common/abi" "$SYNC_ROOT/kernel/common/abi"
cp -R "$REPOSITORY/darwin/api/abi" "$SYNC_ROOT/darwin/api/abi"
cp "$REPOSITORY/darwin/api/include/uml/emulator.h" \
	"$REPOSITORY/darwin/api/include/uml/abi_identity.h" \
	"$SYNC_ROOT/darwin/api/include/uml/"
SYNC="$SYNC_ROOT/kernel/common/abi/sync.sh"
"$SYNC" --host-dir "$SYNC_ROOT/host" --kernel-source "$SYNC_ROOT/linux" \
	>/dev/null
"$SYNC" --host-dir "$SYNC_ROOT/host" --kernel-source "$SYNC_ROOT/linux" \
	--verify >/dev/null
cmp -s "$SYNC_ROOT/darwin/api/include/uml/abi_identity.h" \
	"$REPOSITORY/darwin/api/include/uml/abi_identity.h" &&
	cmp -s "$SYNC_ROOT/host/uml_darwin_abi_identity.h" \
		"$REPOSITORY/darwin/include/uml_darwin_abi_identity.h" &&
	cmp -s "$SYNC_ROOT/linux/arch/um/os-Darwin/uml_darwin_abi_identity.h" \
		"$REPOSITORY/darwin/include/uml_darwin_abi_identity.h" ||
	fail "sync.sh did not reproduce the checked-in identity headers"
for identity in \
	"$SYNC_ROOT/kernel/common/abi/identity.env:DARWIN_UML_ABI_VERSION" \
	"$SYNC_ROOT/darwin/api/abi/identity.env:UML_EMULATOR_ABI_VERSION"; do
	path=${identity%%:*}
	key=${identity#*:}
	cp "$path" "$path.saved"
	sed "s/^$key=.*/$key=999/" "$path.saved" > "$path"
	expect_failure "sync.sh --verify of a stale $key header" \
		"$SYNC" --host-dir "$SYNC_ROOT/host" \
		--kernel-source "$SYNC_ROOT/linux" --verify
	"$SYNC" --host-dir "$SYNC_ROOT/host" --kernel-source "$SYNC_ROOT/linux" \
		>/dev/null
	"$SYNC" --host-dir "$SYNC_ROOT/host" --kernel-source "$SYNC_ROOT/linux" \
		--verify >/dev/null
	grep -Fx "#define $key 999U" \
		"$SYNC_ROOT/darwin/api/include/uml/abi_identity.h" \
		"$SYNC_ROOT/host/uml_darwin_abi_identity.h" \
		"$SYNC_ROOT/linux/arch/um/os-Darwin/uml_darwin_abi_identity.h" \
		>/dev/null || fail "sync.sh did not regenerate $key from identity.env"
	mv "$path.saved" "$path"
done

import_audit()
{
	"$PYTHON" -B "$REPOSITORY/kernel/common/abi/bridge_imports.py" "$@"
}
# nm_listing PAGE SLICE OUTPUT: an `nm -u` listing of exactly the symbols the
# audit expects of that page size and slice.
nm_listing()
{
	import_audit --print-expected --page-size "$1" --slice "$2" > "$3.symbols"
	awk '{ print "                 U " $0 }' "$3.symbols" > "$3"
}
nm_listing 4k arm64 "$TEST_ROOT/4k.nm"
nm_listing 16k arm64 "$TEST_ROOT/16k.nm"
nm_listing 4k x86_64 "$TEST_ROOT/x86-4k.nm"
# The cross-profile rejections below mean something only while the policy
# keeps each profile's set distinct.
! cmp -s "$TEST_ROOT/4k.nm" "$TEST_ROOT/16k.nm" &&
	! cmp -s "$TEST_ROOT/4k.nm" "$TEST_ROOT/x86-4k.nm" &&
	! cmp -s "$TEST_ROOT/16k.nm" "$TEST_ROOT/x86-4k.nm" ||
	fail "the import policy no longer distinguishes the page profiles and slices"
import_audit --nm-output "$TEST_ROOT/4k.nm" --page-size 4k \
	--slice arm64 >/dev/null
import_audit --nm-output "$TEST_ROOT/16k.nm" --page-size 16k \
	--slice arm64 >/dev/null
import_audit --nm-output "$TEST_ROOT/x86-4k.nm" --page-size 4k \
	--slice x86_64 >/dev/null
expect_failure "16 KiB import audit of a 4 KiB-only symbol" \
	import_audit --nm-output "$TEST_ROOT/4k.nm" --page-size 16k \
	--slice arm64
expect_failure "x86_64 import audit of an arm64-only symbol" \
	import_audit --nm-output "$TEST_ROOT/4k.nm" --page-size 4k \
	--slice x86_64
expect_failure "arm64 import audit missing an arm64-only symbol" \
	import_audit --nm-output "$TEST_ROOT/x86-4k.nm" --page-size 4k \
	--slice arm64
sed '1d' "$TEST_ROOT/16k.nm" > "$TEST_ROOT/16k-missing.nm"
expect_failure "16 KiB import audit missing an active symbol" \
	import_audit --nm-output "$TEST_ROOT/16k-missing.nm" \
	--page-size 16k --slice arm64
{ cat "$TEST_ROOT/16k.nm"; printf '%s\n' \
	"                 U _pthread_sigmask"; } > "$TEST_ROOT/16k-thread-mask.nm"
import_audit --nm-output "$TEST_ROOT/16k-thread-mask.nm" \
	--page-size 16k --slice arm64 >/dev/null
for symbol in sigprocmask _sigprocmask 'sigprocmask$UNIX2003' \
	'_sigprocmask$UNIX2003'; do
	{ cat "$TEST_ROOT/16k.nm"; printf '                 U %s\n' "$symbol"; } \
		> "$TEST_ROOT/16k-process-mask.nm"
	expect_diagnostic 'imports the process-wide Darwin sigprocmask' \
		import_audit --nm-output "$TEST_ROOT/16k-process-mask.nm" \
		--page-size 16k --slice arm64
done
# Host code must call darwin_uml_fcntl(); a direct import of the variadic
# Darwin fcntl, in any of its Mach-O spellings, is refused.
for symbol in fcntl _fcntl '_fcntl$NOCANCEL'; do
	{ cat "$TEST_ROOT/16k.nm"; printf '                 U %s\n' "$symbol"; } \
		> "$TEST_ROOT/16k-fcntl.nm"
	expect_diagnostic 'imports the variadic Darwin fcntl' \
		import_audit --nm-output "$TEST_ROOT/16k-fcntl.nm" \
		--page-size 16k --slice arm64
done

expect_failure "import audit without an image" \
	import_audit --page-size 16k --slice arm64
expect_failure "import audit of an unknown page size" \
	import_audit --nm-output "$TEST_ROOT/16k.nm" --page-size 8k \
	--slice arm64
expect_failure "import audit of an unknown slice" \
	import_audit --nm-output "$TEST_ROOT/16k.nm" --page-size 16k \
	--slice riscv64
expect_diagnostic 'x86_64 bridge imports require 4096-byte pages' \
	import_audit --nm-output "$TEST_ROOT/x86-4k.nm" --page-size 16k \
	--slice x86_64
expect_diagnostic 'is missing' \
	import_audit --nm-output "$TEST_ROOT/absent.nm" --page-size 16k \
	--slice arm64
expect_diagnostic 'is missing' \
	import_audit --kernel-binary "$TEST_ROOT/absent" --page-size 16k \
	--slice arm64

# The policy names each conditional symbol once, with a known availability
# and a rationale, and only declared symbols. The copied audit resolves the
# fixture tree as its repository root.
POLICY="$TEST_ROOT/kernel/common/abi/import-policy.tsv"
copied_import_audit()
{
	"$PYTHON" -B "$TEST_ROOT/kernel/common/abi/bridge_imports.py" "$@"
}
cp "$POLICY" "$TEST_ROOT/import-policy.tsv"
for row in 'darwin_uml_fixture_two_fields\t4k-only' \
	'darwin_uml_fixture_availability\t16k-only\tfixture' \
	'darwin_uml_fixture_rationale\t4k-only\t' \
	"$(sed -n 2p "$TEST_ROOT/import-policy.tsv")" \
	'uml_fixture_not_bridge\t4k-only\tfixture'; do
	{ cat "$TEST_ROOT/import-policy.tsv"; printf '%b\n' "$row"; } > "$POLICY"
	expect_diagnostic 'import policy is malformed' \
		copied_import_audit --nm-output "$TEST_ROOT/16k.nm" \
		--page-size 16k --slice arm64
done
{ printf 'symbol\tavailability\n'; sed 1d "$TEST_ROOT/import-policy.tsv"; } \
	> "$POLICY"
expect_diagnostic 'import policy is malformed' \
	copied_import_audit --nm-output "$TEST_ROOT/16k.nm" \
	--page-size 16k --slice arm64
{ cat "$TEST_ROOT/import-policy.tsv"
	printf 'darwin_uml_fixture_undeclared\t4k-only\tfixture\n'; } > "$POLICY"
expect_diagnostic 'import policy names undeclared bridge symbols' \
	copied_import_audit --nm-output "$TEST_ROOT/16k.nm" \
	--page-size 16k --slice arm64
cp "$TEST_ROOT/import-policy.tsv" "$POLICY"
copied_import_audit --nm-output "$TEST_ROOT/16k.nm" --page-size 16k \
	--slice arm64 >/dev/null

printf '%s\n' \
	"independent ABI identity and page-specific import negative tests passed"
