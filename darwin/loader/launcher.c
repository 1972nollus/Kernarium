/* SPDX-License-Identifier: GPL-2.0 */
#include "elf_loader.h"
#include "../include/uml_darwin_runtime.h"

#include <limits.h>
#include <mach/arm/thread_status.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

extern const unsigned char uml_elf_start[]
	__asm("section$start$__DATA$__uml_elf");
extern const unsigned char uml_elf_end[]
	__asm("section$end$__DATA$__uml_elf");

static void host_fault(int signal, siginfo_t *info, void *context_pointer)
{
	ucontext_t *context = context_pointer;
	uintptr_t pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(
		context->uc_mcontext->__ss);

	dprintf(STDERR_FILENO, "Darwin UML host signal %d at pc=%p address=%p\n",
		signal, (void *)pc, info->si_addr);
	_exit(128 + signal);
}

static void install_host_faults(void)
{
	const int signals[] = { SIGABRT, SIGBUS, SIGSEGV, SIGILL };
	struct sigaction action = { };
	size_t index;

	action.sa_sigaction = host_fault;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	for (index = 0; index < sizeof(signals) / sizeof(signals[0]); index++)
		if (sigaction(signals[index], &action, NULL) < 0)
			abort();
}

static int has_argument_prefix(int argc, char **argv, const char *prefix)
{
	size_t length = strlen(prefix);
	int index;

	for (index = 1; index < argc; index++)
		if (strncmp(argv[index], prefix, length) == 0)
			return 1;
	return 0;
}

static int has_argument(int argc, char **argv, const char *argument)
{
	int index;

	for (index = 1; index < argc; index++)
		if (strcmp(argv[index], argument) == 0)
			return 1;
	return 0;
}

static int informational_request(int argc, char **argv)
{
	return has_argument(argc, argv, "--help") ||
		has_argument(argc, argv, "--version") ||
		has_argument(argc, argv, "--showconfig");
}

static int usable_rootfs(const char *path)
{
	return access(path, R_OK | W_OK) == 0;
}

static int locate_rootfs(char path[PATH_MAX])
{
	const char *configured = getenv("UML_ROOTFS");
	char executable[PATH_MAX];
	char resolved[PATH_MAX];
	char *slash;
	uint32_t length = sizeof(executable);

	if (configured != NULL && configured[0] != '\0') {
		if (realpath(configured, path) != NULL && usable_rootfs(path))
			return 0;
		fprintf(stderr, "UML_ROOTFS is not a writable ext2 image: %s\n",
			configured);
		return -1;
	}
	if (_NSGetExecutablePath(executable, &length) == 0 &&
	    realpath(executable, resolved) != NULL) {
		slash = strrchr(resolved, '/');
		if (slash != NULL) {
			*slash = '\0';
			if (snprintf(path, PATH_MAX, "%s/rootfs.ext2", resolved) <
			    PATH_MAX && usable_rootfs(path))
				return 0;
		}
	}
	if (realpath("rootfs.ext2", path) != NULL && usable_rootfs(path))
		return 0;
	fprintf(stderr,
		"rootfs.ext2 was not found beside the executable or in the "
		"current directory\n");
	return -1;
}

static int append_copy(char **arguments, int *count, const char *argument)
{
	arguments[*count] = strdup(argument);
	if (arguments[*count] == NULL)
		return -1;
	(*count)++;
	return 0;
}

static int prepare_kernel_arguments(int argc, char **argv, int *new_argc,
				    char ***new_argv)
{
	char rootfs[PATH_MAX];
	char ubd_argument[PATH_MAX + 6];
	char **arguments;
	int count;
	int index;

	arguments = calloc((size_t)argc + 6, sizeof(*arguments));
	if (arguments == NULL)
		return -1;
	for (index = 0; index < argc; index++)
		arguments[index] = argv[index];
	count = argc;
	if (!has_argument_prefix(argc, argv, "ubd0")) {
		if (locate_rootfs(rootfs) != 0)
			return -1;
		if (snprintf(ubd_argument, sizeof(ubd_argument), "ubd0=%s",
			     rootfs) >= (int)sizeof(ubd_argument) ||
		    append_copy(arguments, &count, ubd_argument) != 0)
			return -1;
	}
	if (!has_argument_prefix(argc, argv, "root=") &&
	    append_copy(arguments, &count, "root=/dev/ubda") != 0)
		return -1;
	if (!has_argument_prefix(argc, argv, "init=") &&
	    append_copy(arguments, &count, "init=/sbin/init") != 0)
		return -1;
	if (!has_argument_prefix(argc, argv, "mem=") &&
	    append_copy(arguments, &count, "mem=256M") != 0)
		return -1;
	if (!has_argument(argc, argv, "ro") && !has_argument(argc, argv, "rw") &&
	    append_copy(arguments, &count, "rw") != 0)
		return -1;
	arguments[count] = NULL;
	*new_argc = count;
	*new_argv = arguments;
	return 0;
}

int main(int argc, char **argv, char **envp)
{
	char **kernel_argv = argv;
	int kernel_argc = argc;

	install_host_faults();
	if (!informational_request(argc, argv) &&
	    prepare_kernel_arguments(argc, argv, &kernel_argc, &kernel_argv) != 0)
		return 1;
#ifndef UML_LOADER_PROBE
	darwin_uml_runtime_init();
#endif
	return uml_elf_run(uml_elf_start, (size_t)(uml_elf_end - uml_elf_start),
			   kernel_argc, kernel_argv, envp);
}
