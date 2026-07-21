/* SPDX-License-Identifier: GPL-2.0 */
#ifndef UML_DARWIN_ELF_LOADER_H
#define UML_DARWIN_ELF_LOADER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int uml_elf_run(const void *image, size_t image_size, int argc, char **argv,
		char **envp);

#ifdef __cplusplus
}
#endif

#endif
