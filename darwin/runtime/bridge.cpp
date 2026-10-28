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
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

using ::uml::runtime::GuestShutdownKind;

static_assert(static_cast<int>(GuestShutdownKind::power_off) ==
	DARWIN_UML_SHUTDOWN_POWER_OFF);
static_assert(static_cast<int>(GuestShutdownKind::halt) ==
	DARWIN_UML_SHUTDOWN_HALT);
static_assert(static_cast<int>(GuestShutdownKind::restart) ==
	DARWIN_UML_SHUTDOWN_RESTART);

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

namespace uml::runtime {

bool process_guest_spent() noexcept
{
	return ::uml::memory::anonymous_shared_backing_registered();
}

} // namespace uml::runtime

int darwin_uml_open(const char *path, int flags, int mode)
{
	/* Darwin UML kernels request this unlinked sparse descriptor solely as an
	 * identity and size token. The memory layer recognizes every duplicate of
	 * the descriptor, lets the kernel's linear physical window establish the
	 * RAM root, and borrows or aliases ranges of that window for consumers.
	 * Physical iOS commits and wires guest-allocatable pages before publishing
	 * them; the token itself is never mapped or written as guest RAM. */
	if (path != nullptr && strcmp(path, anonymous_physical_memory_path) == 0)
		return open_anonymous_backing_token();
	/* Darwin returns EINTR from an open of a regular file when a carrier
	 * signal lands during it. os_open_file() hands that to its caller as a
	 * failure, which the ubd size probe turns into a missing root device, so
	 * an interrupted open is retried here as UML's CATCH_EINTR does. */
	int descriptor;
	do {
		descriptor = open(path, flags, mode);
	} while (descriptor < 0 && errno == EINTR);
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

/*
 * The kernel calls this from machine_restart(), machine_power_off() and
 * machine_halt() before it runs its exitcalls, from a vCPU carrier. The run's
 * context keeps the kind and the command; the log line names them once, as one
 * write so that concurrent output cannot split it.
 */
void darwin_uml_guest_shutdown(int kind, const char *command)
{
	const char *what;
	GuestShutdownKind recorded;
	switch (kind) {
	case DARWIN_UML_SHUTDOWN_POWER_OFF:
		recorded = GuestShutdownKind::power_off;
		what = "guest powered off";
		break;
	case DARWIN_UML_SHUTDOWN_HALT:
		recorded = GuestShutdownKind::halt;
		what = "guest halted";
		break;
	case DARWIN_UML_SHUTDOWN_RESTART:
		recorded = GuestShutdownKind::restart;
		what = "guest requested restart";
		break;
	default:
		dprintf(STDERR_FILENO, "Darwin: guest reported shutdown kind %d\n",
			kind);
		return;
	}
	const std::string_view text = command == nullptr ?
		std::string_view{} :
		std::string_view{command, strnlen(command,
			::uml::runtime::guest_shutdown_command_limit)};
	if (auto *context = ::uml::runtime::current_context();
	    context != nullptr)
		context->record_guest_shutdown(recorded, text);
	/* The guest chose the command: keep control characters out of the log. */
	char line[96 + ::uml::runtime::guest_shutdown_command_limit];
	size_t length = static_cast<size_t>(
		snprintf(line, sizeof(line), "%s", what));
	if (!text.empty()) {
		length += static_cast<size_t>(snprintf(line + length,
			sizeof(line) - length, " (command '"));
		for (const char character : text)
			line[length++] = (character >= ' ' && character != 0x7f) ?
				character : '?';
		line[length++] = '\'';
		line[length++] = ')';
	}
	line[length++] = '\n';
	ssize_t written;
	do {
		written = write(STDERR_FILENO, line, length);
	} while (written < 0 && errno == EINTR);
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
