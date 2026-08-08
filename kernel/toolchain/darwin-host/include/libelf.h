/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_HOST_LIBELF_H
#define UML_DARWIN_HOST_LIBELF_H

/*
 * Linux host tools use <libelf.h> on Darwin only because macOS does not ship
 * an ELF ABI header.  They parse mapped ELF objects directly and do not use
 * the libelf library API.  Reuse the UAPI declarations from the Linux source
 * being built so the layouts always match that source version.
 *
 * kernel/build-kernel.sh supplies the source and generated UAPI directories
 * after the platform include paths.  __EXPORTED_HEADERS__ suppresses the
 * intentional warning emitted when raw UAPI headers are consumed directly.
 */
#ifndef __EXPORTED_HEADERS__
#define __EXPORTED_HEADERS__
#define UML_DARWIN_UNDEFINE_EXPORTED_HEADERS
#endif
#include <linux/elf.h>
#ifdef UML_DARWIN_UNDEFINE_EXPORTED_HEADERS
#undef UML_DARWIN_UNDEFINE_EXPORTED_HEADERS
#undef __EXPORTED_HEADERS__
#endif

/* Extended section indexes are defined by the generic ELF ABI. */
#ifndef SHN_XINDEX
#define SHN_XINDEX 0xffff
#endif
#ifndef SHT_SYMTAB_SHNDX
#define SHT_SYMTAB_SHNDX 18
#endif

/*
 * Linux's UAPI aliases these helpers through ELF_ST_BIND and ELF_ST_TYPE.
 * modpost deliberately undefines those host-word-size aliases before choosing
 * its target ELF class, so keep the class-specific forms self-contained.
 */
#undef ELF32_ST_BIND
#undef ELF32_ST_TYPE
#undef ELF64_ST_BIND
#undef ELF64_ST_TYPE
#undef ELF32_R_INFO
#undef ELF64_R_INFO
#define ELF32_ST_BIND(info) ((info) >> 4)
#define ELF32_ST_TYPE(info) ((info) & 0x0f)
#define ELF64_ST_BIND(info) ((info) >> 4)
#define ELF64_ST_TYPE(info) ((info) & 0x0f)
#define ELF32_R_INFO(symbol, type) (((symbol) << 8) + ((type) & 0xff))
#define ELF64_R_INFO(symbol, type) \
	(((Elf64_Xword)(symbol) << 32) + ((type) & 0xffffffffL))

/*
 * Darwin does not provide the processor-specific relocation namespaces used
 * by Linux host tools.  Keep those build-host-only definitions in this outer
 * compatibility header instead of carrying a shim in the Linux source tree.
 */
#ifndef STT_LOPROC
#define STT_LOPROC 13
#endif
#ifndef STT_SPARC_REGISTER
#define STT_SPARC_REGISTER 13
#endif

#ifndef R_386_NONE
#define R_386_NONE 0
#define R_386_32 1
#define R_386_PC32 2
#endif
#ifndef R_X86_64_NONE
#define R_X86_64_NONE 0
#define R_X86_64_64 1
#endif
#ifndef R_390_64
#define R_390_64 22
#endif
#ifndef R_AARCH64_NONE
#define R_AARCH64_NONE 0
#define R_AARCH64_ABS64 257
#define R_AARCH64_CALL26 283
#endif
#ifndef R_ARM_NONE
#define R_ARM_NONE 0
#define R_ARM_PC24 1
#define R_ARM_ABS32 2
#define R_ARM_REL32 3
#define R_ARM_THM_PC22 10
#define R_ARM_THM_CALL 10
#define R_ARM_CALL 28
#define R_ARM_JUMP24 29
#define R_ARM_THM_JUMP24 30
#define R_ARM_MOVW_ABS_NC 43
#define R_ARM_MOVT_ABS 44
#define R_ARM_THM_MOVW_ABS_NC 47
#define R_ARM_THM_MOVT_ABS 48
#define R_ARM_THM_JUMP19 51
#endif
#ifndef R_LARCH_32
#define R_LARCH_32 1
#define R_LARCH_64 2
#define R_LARCH_MARK_LA 20
#define R_LARCH_SOP_PUSH_PLT_PCREL 29
#define R_LARCH_SUB32 55
#define R_LARCH_RELAX 100
#define R_LARCH_ALIGN 102
#endif
#ifndef R_MIPS_32
#define R_MIPS_32 2
#define R_MIPS_26 4
#define R_MIPS_LO16 6
#define R_MIPS_64 18
#endif
#ifndef R_PPC_ADDR32
#define R_PPC_ADDR32 1
#endif
#ifndef R_PPC64_ADDR64
#define R_PPC64_ADDR64 38
#endif
#ifndef R_RISCV_SUB32
#define R_RISCV_SUB32 39
#endif
#ifndef R_SH_DIR32
#define R_SH_DIR32 1
#endif
#ifndef R_SPARC_64
#define R_SPARC_64 32
#endif

#endif /* UML_DARWIN_HOST_LIBELF_H */
