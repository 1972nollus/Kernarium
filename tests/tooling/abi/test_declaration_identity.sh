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
		"kernel declaration mutation passed without a kernel identity update" >&2
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
		"public declaration mutation passed without a public identity update" >&2
	exit 1
fi

# sync.sh writes each identity header from its identity.env and --verify
# rejects a header that no longer matches. Run the copied tool so it resolves
# the fixture tree as its repository root.
SYNC_ROOT="$WORK_DIRECTORY/sync"
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
		"$REPOSITORY/darwin/include/uml_darwin_abi_identity.h" || {
	printf '%s\n' "sync.sh did not reproduce the checked-in identity headers" >&2
	exit 1
}
for identity in \
	"$SYNC_ROOT/kernel/common/abi/identity.env:DARWIN_UML_ABI_VERSION" \
	"$SYNC_ROOT/darwin/api/abi/identity.env:UML_EMULATOR_ABI_VERSION"; do
	path=${identity%%:*}
	key=${identity#*:}
	cp "$path" "$path.saved"
	sed "s/^$key=.*/$key=999/" "$path.saved" > "$path"
	if "$SYNC" --host-dir "$SYNC_ROOT/host" \
		--kernel-source "$SYNC_ROOT/linux" --verify >/dev/null 2>&1; then
		printf '%s\n' "sync.sh --verify accepted a stale $key header" >&2
		exit 1
	fi
	"$SYNC" --host-dir "$SYNC_ROOT/host" --kernel-source "$SYNC_ROOT/linux" \
		>/dev/null
	"$SYNC" --host-dir "$SYNC_ROOT/host" --kernel-source "$SYNC_ROOT/linux" \
		--verify >/dev/null
	grep -Fx "#define $key 999U" \
		"$SYNC_ROOT/darwin/api/include/uml/abi_identity.h" \
		"$SYNC_ROOT/host/uml_darwin_abi_identity.h" \
		"$SYNC_ROOT/linux/arch/um/os-Darwin/uml_darwin_abi_identity.h" \
		>/dev/null || {
		printf '%s\n' "sync.sh did not regenerate $key from identity.env" >&2
		exit 1
	}
	mv "$path.saved" "$path"
done

IMPORT_VERIFY="$REPOSITORY/kernel/common/abi/verify-linux-imports.sh"
sed -n -E 's/.*(darwin_uml_[A-Za-z0-9_]+)\(.*/\1/p' \
	"$REPOSITORY"/kernel/common/abi/darwin_uml_*.h |
	LC_ALL=C sort -u > "$WORK_DIRECTORY/all-symbols"
awk -F '\t' 'NR > 1 && $2 == "4k-only" { print $1 }' \
	"$REPOSITORY/kernel/common/abi/import-policy.tsv" |
	LC_ALL=C sort > "$WORK_DIRECTORY/4k-only-symbols"
awk -F '\t' 'NR > 1 && $2 == "arm64-only" { print $1 }' \
	"$REPOSITORY/kernel/common/abi/import-policy.tsv" |
	LC_ALL=C sort > "$WORK_DIRECTORY/arm64-only-symbols"
comm -23 "$WORK_DIRECTORY/all-symbols" "$WORK_DIRECTORY/4k-only-symbols" \
	> "$WORK_DIRECTORY/16k-symbols"
comm -23 "$WORK_DIRECTORY/all-symbols" "$WORK_DIRECTORY/arm64-only-symbols" \
	> "$WORK_DIRECTORY/x86-4k-symbols"
awk '{ print "                 U " $0 }' "$WORK_DIRECTORY/all-symbols" \
	> "$WORK_DIRECTORY/4k.nm"
awk '{ print "                 U " $0 }' "$WORK_DIRECTORY/16k-symbols" \
	> "$WORK_DIRECTORY/16k.nm"
awk '{ print "                 U " $0 }' "$WORK_DIRECTORY/x86-4k-symbols" \
	> "$WORK_DIRECTORY/x86-4k.nm"
"$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/4k.nm" --page-size 4k \
	--slice arm64 >/dev/null
"$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/16k.nm" --page-size 16k \
	--slice arm64 >/dev/null
"$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/x86-4k.nm" --page-size 4k \
	--slice x86_64 >/dev/null
if "$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/4k.nm" --page-size 16k \
	--slice arm64 >/dev/null 2>&1; then
	printf '%s\n' "16 KiB import audit accepted a 4 KiB-only symbol" >&2
	exit 1
fi
if "$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/4k.nm" --page-size 4k \
	--slice x86_64 >/dev/null 2>&1; then
	printf '%s\n' "x86_64 import audit accepted an arm64-only symbol" >&2
	exit 1
fi
if "$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/x86-4k.nm" \
	--page-size 4k --slice arm64 >/dev/null 2>&1; then
	printf '%s\n' "arm64 import audit accepted a missing arm64-only symbol" >&2
	exit 1
fi
sed '1d' "$WORK_DIRECTORY/16k.nm" > "$WORK_DIRECTORY/16k-missing.nm"
if "$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/16k-missing.nm" \
	--page-size 16k --slice arm64 >/dev/null 2>&1; then
	printf '%s\n' "16 KiB import audit accepted a missing active symbol" >&2
	exit 1
fi
{ cat "$WORK_DIRECTORY/16k.nm"; printf '%s\n' \
	"                 U _pthread_sigmask"; } > "$WORK_DIRECTORY/16k-thread-mask.nm"
"$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/16k-thread-mask.nm" \
	--page-size 16k --slice arm64 >/dev/null
{ cat "$WORK_DIRECTORY/16k.nm"; printf '%s\n' \
	"                 U sigprocmask"; } > "$WORK_DIRECTORY/16k-process-mask.nm"
if "$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/16k-process-mask.nm" \
	--page-size 16k --slice arm64 >/dev/null 2>&1; then
	printf '%s\n' "import audit accepted the process-wide sigprocmask" >&2
	exit 1
fi
# Host code must call darwin_uml_fcntl(); a direct import of the variadic
# Darwin fcntl, in any of its Mach-O spellings, is refused.
for symbol in fcntl _fcntl '_fcntl$NOCANCEL'; do
	{ cat "$WORK_DIRECTORY/16k.nm"; printf '                 U %s\n' "$symbol"; } \
		> "$WORK_DIRECTORY/16k-fcntl.nm"
	if "$IMPORT_VERIFY" --nm-output "$WORK_DIRECTORY/16k-fcntl.nm" \
		--page-size 16k --slice arm64 >/dev/null 2>"$WORK_DIRECTORY/fcntl.err"; then
		printf '%s\n' "import audit accepted a direct $symbol import" >&2
		exit 1
	fi
	grep -q 'imports the variadic Darwin fcntl' "$WORK_DIRECTORY/fcntl.err" || {
		printf '%s\n' "import audit rejected $symbol for another reason:" >&2
		cat "$WORK_DIRECTORY/fcntl.err" >&2
		exit 1
	}
done

printf '%s\n' \
	"independent ABI identity and page-specific import negative tests passed"
