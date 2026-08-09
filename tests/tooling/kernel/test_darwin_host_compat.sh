#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

REPOSITORY=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
COMPAT_INCLUDE="$REPOSITORY/kernel/toolchain/darwin-host/include"
TOPIC="$REPOSITORY/kernel/patches/series/0008-kbuild-make-host-ELF-tools-portable-to-Darwin.patch"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-darwin-host-compat.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

fail()
{
	printf '%s\n' "test_darwin_host_compat.sh: $*" >&2
	exit 1
}

HOST_CC=$(command -v cc) || fail "cc is unavailable"
mkdir -p -- "$TEST_ROOT/uapi/linux"
cat >"$TEST_ROOT/uapi/linux/elf.h" <<'EOF'
#ifndef UML_DARWIN_HOST_TEST_LINUX_ELF_H
#define UML_DARWIN_HOST_TEST_LINUX_ELF_H
typedef unsigned int Elf32_Word;
typedef unsigned long long Elf64_Xword;
typedef struct {
	unsigned char e_ident[16];
	Elf32_Word e_version;
} Elf32_Ehdr;
#define SHN_XINDEX 0xffff
#define SHT_SYMTAB_SHNDX 18
#define ELF_ST_BIND(info) ((info) >> 4)
#define ELF_ST_TYPE(info) ((info) & 0x0f)
#define ELF32_ST_BIND(info) ELF_ST_BIND(info)
#define ELF32_ST_TYPE(info) ELF_ST_TYPE(info)
#define ELF64_ST_BIND(info) ELF_ST_BIND(info)
#define ELF64_ST_TYPE(info) ELF_ST_TYPE(info)
#define ELF32_R_INFO(symbol, type) (((symbol) << 8) + ((type) & 0xff))
#define ELF64_R_INFO(symbol, type) \
	(((Elf64_Xword)(symbol) << 32) + ((type) & 0xffffffffL))
#endif
EOF

cat >"$TEST_ROOT/compat.c" <<'EOF'
#include <elf.h>
#include <libelf.h>
#include <byteswap.h>

#ifndef UML_DARWIN_HOST_ELF_H
#error "the repository elf.h wrapper was not selected"
#endif
#ifndef UML_DARWIN_HOST_LIBELF_H
#error "the repository libelf.h compatibility contract was not selected"
#endif
#ifndef UML_DARWIN_HOST_BYTESWAP_H
#error "the repository byteswap.h wrapper was not selected"
#endif

_Static_assert(sizeof(Elf32_Ehdr) == 20, "ELF declarations did not come from UAPI");
_Static_assert(SHN_XINDEX == 0xffff, "extended section index changed");
_Static_assert(SHT_SYMTAB_SHNDX == 18, "section-index type changed");
_Static_assert(ELF32_ST_BIND(0xab) == 0x0a, "ELF32 binding changed");
_Static_assert(ELF64_ST_TYPE(0xab) == 0x0b, "ELF64 type changed");
_Static_assert(ELF32_R_INFO(0x1234, 0x56) == 0x123456,
	"ELF32 relocation packing changed");
_Static_assert(ELF64_R_INFO(0x12345678, 0x9abcdef0) ==
	0x123456789abcdef0ULL, "ELF64 relocation packing changed");
_Static_assert(R_AARCH64_CALL26 == 283, "AArch64 relocation ABI changed");
_Static_assert(R_ARM_THM_JUMP19 == 51, "ARM relocation ABI changed");
_Static_assert(bswap_16(0x1234) == 0x3412, "16-bit byte swap changed");
_Static_assert(bswap_32(0x12345678U) == 0x78563412U,
	"32-bit byte swap changed");
_Static_assert(bswap_64(0x0123456789abcdefULL) == 0xefcdab8967452301ULL,
	"64-bit byte swap changed");

int main(void)
{
	return 0;
}
EOF

"$HOST_CC" -std=gnu11 -Wall -Wextra -Werror \
	-I"$COMPAT_INCLUDE" -I"$TEST_ROOT/uapi" \
	"$TEST_ROOT/compat.c" -o "$TEST_ROOT/compat"
"$TEST_ROOT/compat"

git -C "$REPOSITORY" apply --numstat "$TOPIC" |
	awk -F '\t' '{ print $3 }' | LC_ALL=C sort >"$TEST_ROOT/actual-paths"
cat >"$TEST_ROOT/expected-paths" <<'EOF'
scripts/Makefile.vmlinux
scripts/link-vmlinux.sh
scripts/mod/file2alias.c
EOF
cmp -s "$TEST_ROOT/expected-paths" "$TEST_ROOT/actual-paths" || {
	printf '%s\n' 'expected retained topic paths:' >&2
	cat "$TEST_ROOT/expected-paths" >&2
	printf '%s\n' 'actual retained topic paths:' >&2
	cat "$TEST_ROOT/actual-paths" >&2
	fail "topic 0008 regained compatibility-only Linux source changes"
}

grep -F 'cmd_trim_modinfo_padding = perl -0777' "$TOPIC" >/dev/null ||
	fail "topic 0008 lost Darwin modinfo trimming"
grep -F 'ldlibs=""' "$TOPIC" >/dev/null ||
	fail "topic 0008 lost Darwin vmlinux link-library selection"
grep -F '#define uuid_t linux_uuid_t' "$TOPIC" >/dev/null ||
	fail "topic 0008 lost the Darwin file2alias uuid_t collision guard"

printf '%s\n' "Darwin host compatibility header and topic-boundary tests passed"
