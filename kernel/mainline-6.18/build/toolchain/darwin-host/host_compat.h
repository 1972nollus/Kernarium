/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/*
 * Compatibility injected only into Linux host tools on Darwin. Keeping these
 * definitions outside the Linux source avoids carrying build-host policy in
 * the maintained kernel delta.
 */
#ifndef __linux__
#include <errno.h>
#include <sys/types.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

static inline ssize_t uml_darwin_copy_file_range(
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

#define copy_file_range uml_darwin_copy_file_range
#endif
