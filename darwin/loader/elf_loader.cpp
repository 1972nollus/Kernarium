/* SPDX-License-Identifier: GPL-2.0 */
#include "elf_loader.h"
#include "../include/uml_elf64.h"

#include <libkern/OSCacheControl.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <vector>

struct dynamic_info {
	const uml_elf64_sym *symtab;
	const char *strtab;
	size_t strsz;
	const uml_elf64_rela *rela;
	size_t rela_count;
	const uml_elf64_rela *jmprel;
	size_t jmprel_count;
	size_t syment;
	size_t relaent;
};

static uintptr_t align_down(uintptr_t value, uintptr_t alignment)
{
	return value & ~(alignment - 1);
}

static uintptr_t align_up(uintptr_t value, uintptr_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static int range_inside(size_t image_size, uint64_t offset, uint64_t length)
{
	return offset <= image_size && length <= image_size - offset;
}

static int segment_protection(uint32_t flags)
{
	int protection = 0;

	if (flags & UML_PF_R)
		protection |= PROT_READ;
	if (flags & UML_PF_W)
		protection |= PROT_WRITE;
	if (flags & UML_PF_X)
		protection |= PROT_EXEC;
	return protection;
}

static void *resolve_symbol(const char *name)
{
	void *address = dlsym(RTLD_DEFAULT, name);

	/* Darwin SDK headers give a few libc entry points an asm-label alias. */
	if (address == NULL && name[0] == '_')
		address = dlsym(RTLD_DEFAULT, name + 1);
	return address;
}

static int load_dynamic_info(uintptr_t bias, const uml_elf64_dyn *dynamic,
			     struct dynamic_info *info)
{
	uint64_t relasz = 0, pltrelsz = 0;

	memset(info, 0, sizeof(*info));
	for (; dynamic->tag != UML_DT_NULL; dynamic++) {
		switch (dynamic->tag) {
		case UML_DT_SYMTAB:
			info->symtab = reinterpret_cast<const uml_elf64_sym *>(
				bias + dynamic->un.pointer);
			break;
		case UML_DT_STRTAB:
			info->strtab = reinterpret_cast<const char *>(
				bias + dynamic->un.pointer);
			break;
		case UML_DT_STRSZ:
			info->strsz = dynamic->un.value;
			break;
		case UML_DT_SYMENT:
			info->syment = dynamic->un.value;
			break;
		case UML_DT_RELA:
			info->rela = reinterpret_cast<const uml_elf64_rela *>(
				bias + dynamic->un.pointer);
			break;
		case UML_DT_RELASZ:
			relasz = dynamic->un.value;
			break;
		case UML_DT_RELAENT:
			info->relaent = dynamic->un.value;
			break;
		case UML_DT_JMPREL:
			info->jmprel = reinterpret_cast<const uml_elf64_rela *>(
				bias + dynamic->un.pointer);
			break;
		case UML_DT_PLTRELSZ:
			pltrelsz = dynamic->un.value;
			break;
		case UML_DT_PLTREL:
			if (dynamic->un.value != UML_DT_RELA_VALUE) {
				fprintf(stderr, "UML ELF: non-RELA PLT relocations\n");
				return -1;
			}
			break;
		case UML_DT_RELR:
		case UML_DT_RELRSZ:
		case UML_DT_RELRENT:
			fprintf(stderr, "UML ELF: packed RELR is not supported\n");
			return -1;
		default:
			break;
		}
	}

	if (info->relaent == 0)
		info->relaent = sizeof(uml_elf64_rela);
	if (info->syment == 0)
		info->syment = sizeof(uml_elf64_sym);
	if (info->relaent != sizeof(uml_elf64_rela) ||
	    info->syment != sizeof(uml_elf64_sym)) {
		fprintf(stderr, "UML ELF: unexpected dynamic entry size\n");
		return -1;
	}
	info->rela_count = relasz / info->relaent;
	info->jmprel_count = pltrelsz / info->relaent;
	return 0;
}

static int apply_relocations(uintptr_t bias, const struct dynamic_info *info,
			     const uml_elf64_rela *relocations, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		const uml_elf64_rela *rela = &relocations[i];
		uint32_t type = UML_ELF64_R_TYPE(rela->info);
		uint32_t symbol_index = UML_ELF64_R_SYM(rela->info);
		auto *destination = reinterpret_cast<uintptr_t *>(bias + rela->offset);
		uintptr_t value;

		if (type == UML_R_AARCH64_RELATIVE) {
			*destination = bias + rela->addend;
			continue;
		}

		if (type != UML_R_AARCH64_ABS64 &&
		    type != UML_R_AARCH64_GLOB_DAT &&
		    type != UML_R_AARCH64_JUMP_SLOT) {
			fprintf(stderr, "UML ELF: unsupported relocation %u\n", type);
			return -1;
		}
		if (info->symtab == NULL || info->strtab == NULL) {
			fprintf(stderr, "UML ELF: relocation has no symbol table\n");
			return -1;
		}

		const uml_elf64_sym *symbol = &info->symtab[symbol_index];
		if (symbol->name >= info->strsz) {
			fprintf(stderr, "UML ELF: invalid symbol name offset\n");
			return -1;
		}
		if (symbol->shndx != UML_SHN_UNDEF) {
			value = bias + symbol->value;
		} else {
			const char *name = info->strtab + symbol->name;
			void *resolved = resolve_symbol(name);

			if (resolved == NULL && UML_ELF64_ST_BIND(symbol->info) != UML_STB_WEAK) {
				fprintf(stderr, "UML ELF: unresolved Darwin symbol: %s\n",
					name);
				return -1;
			}
			value = (uintptr_t)resolved;
		}
		*destination = value + rela->addend;
	}
	return 0;
}

int uml_elf_run(const void *image, size_t image_size, int argc, char **argv,
		char **envp)
{
	const auto *bytes = static_cast<const unsigned char *>(image);
	const auto *header = static_cast<const uml_elf64_ehdr *>(image);
	const uml_elf64_phdr *programs;
	const uml_elf64_dyn *dynamic = NULL;
	struct dynamic_info dynamic_info;
	uintptr_t minimum = UINTPTR_MAX, maximum = 0, bias;
	size_t page_size = (size_t)getpagesize();
	size_t page_count;
	void *reservation;
	int (*entry)(int, char **, char **);
	uint16_t i;

	if (image_size < sizeof(*header) || header->ident[0] != UML_ELFMAG0 ||
	    header->ident[1] != UML_ELFMAG1 || header->ident[2] != UML_ELFMAG2 ||
	    header->ident[3] != UML_ELFMAG3 ||
	    header->ident[4] != UML_ELFCLASS64 ||
	    header->ident[5] != UML_ELFDATA2LSB || header->type != UML_ET_DYN ||
	    header->machine != UML_EM_AARCH64 ||
	    header->phentsize != sizeof(uml_elf64_phdr) ||
	    !range_inside(image_size, header->phoff,
			  (uint64_t)header->phnum * header->phentsize)) {
		fprintf(stderr, "UML ELF: invalid AArch64 ET_DYN image\n");
		return 126;
	}
	programs = reinterpret_cast<const uml_elf64_phdr *>(bytes + header->phoff);

	for (i = 0; i < header->phnum; i++) {
		const uml_elf64_phdr *program = &programs[i];

		if (program->type != UML_PT_LOAD)
			continue;
		if (program->filesz > program->memsz ||
		    !range_inside(image_size, program->offset, program->filesz)) {
			fprintf(stderr, "UML ELF: invalid load segment\n");
			return 126;
		}
		if (align_down(program->vaddr, page_size) < minimum)
			minimum = align_down(program->vaddr, page_size);
		if (align_up(program->vaddr + program->memsz, page_size) > maximum)
			maximum = align_up(program->vaddr + program->memsz, page_size);
	}
	if (minimum == UINTPTR_MAX || maximum <= minimum) {
		fprintf(stderr, "UML ELF: image has no loadable segments\n");
		return 126;
	}

	/*
	 * UML's link-time memory layout is part of its physical/virtual address
	 * contract.  Loading it at a random PIE bias would make __pa(), brk, and
	 * the guest/kernel split disagree, so reserve the linked high window.
	 */
	reservation = mmap((void *)minimum, maximum - minimum, PROT_NONE,
			   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	if (reservation == MAP_FAILED) {
		perror("UML ELF mmap");
		return 126;
	}
	if ((uintptr_t)reservation != minimum) {
		fprintf(stderr, "UML ELF: preferred kernel address unavailable\n");
		return 126;
	}
	bias = 0;
	page_count = (maximum - minimum) / page_size;
	std::vector<unsigned char> page_protections(page_count);

	for (i = 0; i < header->phnum; i++) {
		const uml_elf64_phdr *program = &programs[i];
		uintptr_t start, end;

		if (program->type != UML_PT_LOAD)
			continue;
		start = align_down(bias + program->vaddr, page_size);
		end = align_up(bias + program->vaddr + program->memsz, page_size);
		if (mprotect((void *)start, end - start, PROT_READ | PROT_WRITE) != 0) {
			perror("UML ELF mprotect load");
			return 126;
		}
		memcpy((void *)(bias + program->vaddr), bytes + program->offset,
		       program->filesz);
		memset((void *)(bias + program->vaddr + program->filesz), 0,
		       program->memsz - program->filesz);
		if (program->type == UML_PT_DYNAMIC)
			dynamic = reinterpret_cast<const uml_elf64_dyn *>(
				bias + program->vaddr);
	}
	/* PT_DYNAMIC is normally nested in a PT_LOAD, so locate it separately. */
	for (i = 0; i < header->phnum; i++) {
		if (programs[i].type == UML_PT_DYNAMIC)
			dynamic = reinterpret_cast<const uml_elf64_dyn *>(
				bias + programs[i].vaddr);
	}
	if (dynamic != NULL) {
		if (load_dynamic_info(bias, dynamic, &dynamic_info) != 0 ||
		    apply_relocations(bias, &dynamic_info, dynamic_info.rela,
				      dynamic_info.rela_count) != 0 ||
		    apply_relocations(bias, &dynamic_info, dynamic_info.jmprel,
				      dynamic_info.jmprel_count) != 0)
			return 126;
	}

	for (i = 0; i < header->phnum; i++) {
		const uml_elf64_phdr *program = &programs[i];
		uintptr_t start, end;
		size_t page;

		if (program->type != UML_PT_LOAD)
			continue;
		start = align_down(bias + program->vaddr, page_size);
		end = align_up(bias + program->vaddr + program->memsz, page_size);
		for (page = (start - minimum) / page_size;
		     page < (end - minimum) / page_size; page++)
			page_protections[page] |= segment_protection(program->flags);
	}
	for (size_t page = 0; page < page_count; page++) {
		uintptr_t address = minimum + page * page_size;
		int protection = page_protections[page];

		/* lld may place tiny .exit.text beside writable init metadata. */
		if ((protection & (PROT_WRITE | PROT_EXEC)) ==
		    (PROT_WRITE | PROT_EXEC))
			protection &= ~PROT_EXEC;
		if (protection & PROT_EXEC)
			sys_icache_invalidate((void *)address, page_size);
		if (mprotect((void *)address, page_size,
			     protection) != 0) {
			perror("UML ELF mprotect final");
			return 126;
		}
	}
	entry = reinterpret_cast<int (*)(int, char **, char **)>(
		bias + header->entry);
	return entry(argc, argv, envp);
}
