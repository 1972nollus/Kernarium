/* SPDX-License-Identifier: GPL-2.0 */
#include "uml_darwin_host.h"

#include "context.hpp"

#include "device/sockets/socket_paths.hpp"
#include "guest_files.hpp"

#include <atomic>
#include <errno.h>
#include <fcntl.h>
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

int open_anonymous_shared_memory() noexcept
{
	static std::atomic_uint sequence{0};
	char name[64];

	for (unsigned int attempt = 0; attempt != 16; ++attempt) {
		const unsigned int suffix =
			sequence.fetch_add(1, std::memory_order_relaxed);
		const int length = snprintf(name, sizeof(name),
			"/uml-phys-%ld-%u", static_cast<long>(getpid()), suffix);
		if (length < 0 || static_cast<size_t>(length) >= sizeof(name)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		const int descriptor = shm_open(name,
			O_RDWR | O_CREAT | O_EXCL, 0600);
		if (descriptor < 0) {
			if (errno == EEXIST)
				continue;
			return -1;
		}
		if (shm_unlink(name) == 0)
			return descriptor;
		const int error = errno;
		(void)close(descriptor);
		errno = error;
		return -1;
	}
	errno = EEXIST;
	return -1;
}

} // namespace

int darwin_uml_open(const char *path, int flags, int mode)
{
	/* A diagnostic ACK can request a POSIX shared-memory object through the
	 * existing fixed-arity open bridge. It retains the fd/offset aliasing
	 * contract but is anonymous VM rather than an APFS regular file. */
	if (path != nullptr && strcmp(path, anonymous_physical_memory_path) == 0)
		return open_anonymous_shared_memory();
	const int descriptor = open(path, flags, mode);
	::uml::platform::record_guest_file(descriptor, flags);
	return descriptor;
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
