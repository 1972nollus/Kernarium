/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_HOST_ELF_H
#define UML_DARWIN_HOST_ELF_H

/*
 * Kbuild host tools include <elf.h> in the unmodified Linux source. Darwin
 * does not provide that header, so resolve the standard name to the
 * repository-owned Linux-UAPI-backed compatibility contract.
 */
#include <libelf.h>

#endif /* UML_DARWIN_HOST_ELF_H */
