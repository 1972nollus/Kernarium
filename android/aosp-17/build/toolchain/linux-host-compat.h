/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*
 * The checked ACK host sysroot uses glibc 2.17, which predates the
 * copy_file_range declaration. gen_init_cpio treats ENOSYS as its request to
 * use the ordinary read/write fallback, so expose that exact behavior to
 * builder-host tools without changing the ACK source tree.
 */
#include <errno.h>
#include <sys/types.h>

static inline ssize_t uml_android17_copy_file_range(
	int input, off_t *input_offset, int output, off_t *output_offset,
	size_t length, unsigned int flags)
{
	(void)input;
	(void)input_offset;
	(void)output;
	(void)output_offset;
	(void)length;
	(void)flags;
	errno = ENOSYS;
	return -1;
}

#define copy_file_range uml_android17_copy_file_range
