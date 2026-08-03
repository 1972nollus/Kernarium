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

#endif /* UML_DARWIN_HOST_LIBELF_H */
