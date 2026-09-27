#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Require the synchronized bridge to match the Linux image's actual imports.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
KERNEL_BINARY=''
NM_TOOL=${NM:-nm}
NM_OUTPUT=''
PAGE_SIZE=''
KERNEL_SLICE=arm64
IMPORT_POLICY="$ROOT_DIR/kernel/common/abi/import-policy.tsv"

fail()
{
	printf '%s\n' "kernel/common/abi/verify-linux-imports.sh: $*" >&2
	exit 1
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--kernel-binary)
		[ "$#" -ge 2 ] || fail "--kernel-binary requires a path"
		KERNEL_BINARY=$2
		shift 2
		;;
	--nm)
		[ "$#" -ge 2 ] || fail "--nm requires a command"
		NM_TOOL=$2
		shift 2
		;;
	--nm-output)
		[ "$#" -ge 2 ] || fail "--nm-output requires a path"
		NM_OUTPUT=$2
		shift 2
		;;
	--page-size)
		[ "$#" -ge 2 ] || fail "--page-size requires 16k or 4k"
		PAGE_SIZE=$2
		shift 2
		;;
	--slice)
		[ "$#" -ge 2 ] || fail "--slice requires arm64 or x86_64"
		KERNEL_SLICE=$2
		shift 2
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$KERNEL_BINARY" ] || [ -n "$NM_OUTPUT" ] ||
	fail "--kernel-binary or --nm-output is required"
case "$PAGE_SIZE" in
16k|4k) ;;
*) fail "--page-size must be 16k or 4k" ;;
esac
case "$KERNEL_SLICE" in
arm64|x86_64) ;;
*) fail "--slice must be arm64 or x86_64" ;;
esac
[ "$KERNEL_SLICE" != x86_64 ] || [ "$PAGE_SIZE" = 4k ] ||
	fail "the x86_64 slice supports only 4k imports"
[ -f "$IMPORT_POLICY" ] || fail "import policy is missing: $IMPORT_POLICY"
if [ -n "$KERNEL_BINARY" ]; then
	[ -f "$KERNEL_BINARY" ] || fail "kernel binary is missing: $KERNEL_BINARY"
	command -v "$NM_TOOL" >/dev/null 2>&1 || fail "nm is unavailable: $NM_TOOL"
fi

WORK_DIRECTORY=$(mktemp -d "${TMPDIR:-/tmp}/uml-linux-imports.XXXXXX")
cleanup()
{
	rm -f -- "$WORK_DIRECTORY/declared" \
		"$WORK_DIRECTORY/4k-only.unsorted" \
		"$WORK_DIRECTORY/4k-only" \
		"$WORK_DIRECTORY/arm64-only.unsorted" \
		"$WORK_DIRECTORY/arm64-only" \
		"$WORK_DIRECTORY/conditional" \
		"$WORK_DIRECTORY/expected.intermediate" \
		"$WORK_DIRECTORY/expected" \
		"$WORK_DIRECTORY/nm" "$WORK_DIRECTORY/imported"
	rmdir -- "$WORK_DIRECTORY" 2>/dev/null || true
}
trap cleanup 0 HUP INT TERM

sed -n -E 's/.*(darwin_uml_[A-Za-z0-9_]+)\(.*/\1/p' \
	"$ROOT_DIR"/kernel/common/abi/darwin_uml_*.h |
	LC_ALL=C sort -u > "$WORK_DIRECTORY/declared"

if ! awk -F '\t' '
	NR == 1 {
		if ($0 != "symbol\tavailability\trationale")
			exit 1
		next
	}
	NF != 3 || $1 !~ /^darwin_uml_[A-Za-z0-9_]+$/ ||
	($2 != "4k-only" && $2 != "arm64-only") ||
	$3 == "" || seen[$1]++ { exit 1 }
' "$IMPORT_POLICY"; then
	fail "import policy is malformed: $IMPORT_POLICY"
fi
: > "$WORK_DIRECTORY/4k-only.unsorted"
: > "$WORK_DIRECTORY/arm64-only.unsorted"
awk -F '\t' 'NR > 1 && $2 == "4k-only" { print $1 }' "$IMPORT_POLICY" \
	> "$WORK_DIRECTORY/4k-only.unsorted"
awk -F '\t' 'NR > 1 && $2 == "arm64-only" { print $1 }' "$IMPORT_POLICY" \
	> "$WORK_DIRECTORY/arm64-only.unsorted"
LC_ALL=C sort "$WORK_DIRECTORY/4k-only.unsorted" \
	> "$WORK_DIRECTORY/4k-only"
LC_ALL=C sort "$WORK_DIRECTORY/arm64-only.unsorted" \
	> "$WORK_DIRECTORY/arm64-only"
LC_ALL=C sort -u "$WORK_DIRECTORY/4k-only" \
	"$WORK_DIRECTORY/arm64-only" > "$WORK_DIRECTORY/conditional"
undeclared_policy=$(comm -23 "$WORK_DIRECTORY/conditional" \
	"$WORK_DIRECTORY/declared")
[ -z "$undeclared_policy" ] ||
	fail "import policy names undeclared bridge symbols: $undeclared_policy"

if [ "$PAGE_SIZE" = 16k ]; then
	comm -23 "$WORK_DIRECTORY/declared" "$WORK_DIRECTORY/4k-only" \
		> "$WORK_DIRECTORY/expected.intermediate"
else
	cp -- "$WORK_DIRECTORY/declared" \
		"$WORK_DIRECTORY/expected.intermediate"
fi
if [ "$KERNEL_SLICE" = x86_64 ]; then
	comm -23 "$WORK_DIRECTORY/expected.intermediate" \
		"$WORK_DIRECTORY/arm64-only" > "$WORK_DIRECTORY/expected"
else
	cp -- "$WORK_DIRECTORY/expected.intermediate" \
		"$WORK_DIRECTORY/expected"
fi

if [ -n "$NM_OUTPUT" ]; then
	[ -f "$NM_OUTPUT" ] || fail "nm output is missing: $NM_OUTPUT"
	cp -- "$NM_OUTPUT" "$WORK_DIRECTORY/nm"
else
	"$NM_TOOL" -u "$KERNEL_BINARY" > "$WORK_DIRECTORY/nm"
fi
awk '{
	for (field = 1; field <= NF; ++field)
		if ($field ~ /^darwin_uml_[A-Za-z0-9_]+$/)
			print $field
}' "$WORK_DIRECTORY/nm" | LC_ALL=C sort -u > "$WORK_DIRECTORY/imported"

# Darwin's sigprocmask() sets the mask of every thread in the process, vCPU
# carriers included; host code must change a mask with pthread_sigmask().
if awk '{
	for (field = 1; field <= NF; ++field)
		if ($field ~ /^_?sigprocmask([$].*)?$/)
			found = 1
} END { exit !found }' "$WORK_DIRECTORY/nm"; then
	fail "the Linux image imports the process-wide Darwin sigprocmask"
fi
# Apple's arm64 ABI passes variadic arguments on the stack and the kernel's
# host objects pass them in registers, so a direct fcntl() hands libc an
# arbitrary third argument; host code calls darwin_uml_fcntl() instead.
if awk '{
	for (field = 1; field <= NF; ++field)
		if ($field ~ /^_?fcntl([$].*)?$/)
			found = 1
} END { exit !found }' "$WORK_DIRECTORY/nm"; then
	fail "the Linux image imports the variadic Darwin fcntl"
fi

cmp -s "$WORK_DIRECTORY/expected" "$WORK_DIRECTORY/imported" || {
	printf '%s\n' "active declared-only bridge symbols:" >&2
	comm -23 "$WORK_DIRECTORY/expected" "$WORK_DIRECTORY/imported" >&2
	printf '%s\n' "Linux-imported but inactive or undeclared bridge symbols:" >&2
	comm -13 "$WORK_DIRECTORY/expected" "$WORK_DIRECTORY/imported" >&2
	fail "$PAGE_SIZE bridge declarations do not match Linux imports"
}
printf '%s\n' \
	"verified $KERNEL_SLICE $PAGE_SIZE Linux imports exactly match the active synchronized bridge"
