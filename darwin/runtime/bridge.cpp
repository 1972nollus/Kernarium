/* SPDX-License-Identifier: GPL-2.0 */
#include "uml_darwin_host.h"

#include "context.hpp"

#include "device/sockets/socket_paths.hpp"
#include "guest_files.hpp"
#include "memory/shared_backing.hpp"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr char anonymous_physical_memory_path[] =
	"uml:anonymous-physical-memory";

int open_anonymous_backing_token() noexcept
{
	const char *temporary_directory = getenv("TMPDIR");
	if (temporary_directory == nullptr || temporary_directory[0] == '\0')
		temporary_directory = "/tmp";
	char name[PATH_MAX];
	const int length = snprintf(name, sizeof(name),
		"%s/uml-phys-token-XXXXXX", temporary_directory);
	if (length < 0 || static_cast<size_t>(length) >= sizeof(name)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	const int descriptor = mkstemp(name);
	if (descriptor < 0)
		return -1;
	if (unlink(name) == 0 &&
	    ::uml::memory::register_anonymous_shared_backing(descriptor))
		return descriptor;
	const int error = errno;
	(void)close(descriptor);
	errno = error;
	return -1;
}

} // namespace

int darwin_uml_open(const char *path, int flags, int mode)
{
	/* Android ACK requests this unlinked sparse descriptor solely as an
	 * identity and size token. The memory layer recognizes every duplicate of
	 * the descriptor and aliases one anonymous Mach VM object. Physical iOS
	 * commits and wires that object before publishing it; the token itself is
	 * never mapped or written as guest RAM. */
	if (path != nullptr && strcmp(path, anonymous_physical_memory_path) == 0)
		return open_anonymous_backing_token();
	const int descriptor = open(path, flags, mode);
	::uml::platform::record_guest_file(descriptor, flags);
	return descriptor;
}

int darwin_uml_fcntl(int fd, int command, long argument)
{
	if (command == F_PUNCHHOLE &&
	    ::uml::memory::is_anonymous_shared_backing(fd)) {
		errno = EOPNOTSUPP;
		return -1;
	}
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
	::uml::device::remove_registered_socket_paths();
	signal(SIGABRT, SIG_DFL);
	abort();
}

/*
 * Every termination the guest kernel drives arrives here, including the
 * SIGINT and SIGTERM its own fatal handler converts into _exit().  No host
 * destructor runs past this point, so bound socket paths have to be released
 * now or they outlive the process and block the next run on the same names.
 */
void darwin_uml_exit(int status, void *caller)
{
	dprintf(STDERR_FILENO, "Darwin: ELF exit(%d) called from %p\n",
		status, caller);
	::uml::device::remove_registered_socket_paths();
	_exit(status);
}
