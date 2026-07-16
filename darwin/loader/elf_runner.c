/* SPDX-License-Identifier: GPL-2.0 */
#include "elf_loader.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv, char **envp)
{
	struct stat status;
	void *image;
	int fd, result;

	if (argc < 2) {
		fprintf(stderr, "usage: %s AARCH64-ELF [args ...]\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDONLY);
	if (fd < 0 || fstat(fd, &status) != 0) {
		perror(argv[1]);
		return 2;
	}
	image = mmap(NULL, (size_t)status.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (image == MAP_FAILED) {
		perror("mmap payload");
		return 2;
	}
	result = uml_elf_run(image, (size_t)status.st_size, argc - 1, argv + 1,
			     envp);
	munmap(image, (size_t)status.st_size);
	close(fd);
	return result;
}
