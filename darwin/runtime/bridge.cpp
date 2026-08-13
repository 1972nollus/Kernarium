/* SPDX-License-Identifier: GPL-2.0 */
#include "uml_darwin_host.h"

#include "context.hpp"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int darwin_uml_open(const char *path, int flags, int mode)
{
	return open(path, flags, mode);
}

int darwin_uml_fcntl(int fd, int command, long argument)
{
	return fcntl(fd, command, argument);
}

int darwin_uml_ioctl(int fd, unsigned long command, void *argument)
{
	return ioctl(fd, command, argument);
}

int darwin_uml_stop_fd(void)
{
	auto *context = uml::runtime::current_context();
	return context == nullptr ? -1 : context->stop_notification_fd();
}

int darwin_uml_stop_requested(void)
{
	auto *context = uml::runtime::current_context();
	return context != nullptr && context->stop_requested();
}

void darwin_uml_abort(void)
{
	signal(SIGABRT, SIG_DFL);
	abort();
}

void darwin_uml_exit(int status, void *caller)
{
	dprintf(STDERR_FILENO, "Darwin: ELF exit(%d) called from %p\n",
		status, caller);
	_exit(status);
}
