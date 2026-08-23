// SPDX-License-Identifier: GPL-2.0-or-later

#include "socket_paths.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

void require(bool condition)
{
	if (!condition)
		std::abort();
}

template<typename Operation>
void requireThrows(Operation &&operation)
{
	try {
		operation();
	} catch (const std::exception &) {
		return;
	}
	std::abort();
}

std::filesystem::path scratchDirectory()
{
	static const auto directory = [] {
		auto candidate = std::filesystem::temp_directory_path() /
			("uml-socket-path-tests-" + std::to_string(::getpid()));
		std::filesystem::create_directories(candidate);
		return candidate;
	}();
	return directory;
}

bool socketExists(const std::filesystem::path &path)
{
	struct stat status {};
	return ::lstat(path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode);
}

/* Binds a listening Unix socket at `path` and returns its descriptor. */
int bindSocket(const std::filesystem::path &path)
{
	const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
	require(descriptor >= 0);
	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	const auto text = path.string();
	require(text.size() < sizeof(address.sun_path));
	std::memcpy(address.sun_path, text.c_str(), text.size() + 1U);
	require(::bind(descriptor,
		       reinterpret_cast<const sockaddr *>(&address),
		       sizeof(address)) == 0);
	return descriptor;
}

void anAdoptedPathIsRemovedWhenTheGuardIsDestroyed()
{
	const auto path = scratchDirectory() / "destroyed.sock";
	std::filesystem::remove(path);
	const int descriptor = bindSocket(path);
	{
		::uml::device::SocketPath guard {path};
		guard.adopt();
		require(socketExists(path));
	}
	require(!socketExists(path));
	(void)::close(descriptor);
}

void anUnadoptedPathIsLeftAlone()
{
	const auto path = scratchDirectory() / "unadopted.sock";
	std::filesystem::remove(path);
	const int descriptor = bindSocket(path);
	{
		/* bind() failed in the caller, so the guard never took
		 * ownership and must not remove a path it does not own. */
		::uml::device::SocketPath guard {path};
	}
	require(socketExists(path));
	(void)::close(descriptor);
	std::filesystem::remove(path);
}

void theSignalPathRemovesEveryAdoptedSocket()
{
	const auto first = scratchDirectory() / "signal-first.sock";
	const auto second = scratchDirectory() / "signal-second.sock";
	std::filesystem::remove(first);
	std::filesystem::remove(second);
	const int firstDescriptor = bindSocket(first);
	const int secondDescriptor = bindSocket(second);
	::uml::device::SocketPath firstGuard {first};
	::uml::device::SocketPath secondGuard {second};
	firstGuard.adopt();
	secondGuard.adopt();
	require(socketExists(first) && socketExists(second));

	/* What the terminating-signal handler runs.  Neither guard unwinds in
	 * that path, so this call alone has to release both. */
	::uml::device::remove_registered_socket_paths();
	require(!socketExists(first) && !socketExists(second));
	(void)::close(firstDescriptor);
	(void)::close(secondDescriptor);
}

void aReplacedPathSurvives()
{
	const auto path = scratchDirectory() / "replaced.sock";
	std::filesystem::remove(path);
	const int original = bindSocket(path);
	::uml::device::SocketPath guard {path};
	guard.adopt();

	/* Another process removed our socket and bound its own at the same
	 * name.  The recorded device and inode no longer match, so neither the
	 * signal path nor the destructor may remove the replacement. */
	std::filesystem::remove(path);
	const int replacement = bindSocket(path);
	::uml::device::remove_registered_socket_paths();
	require(socketExists(path));
	(void)::close(original);
	(void)::close(replacement);
	std::filesystem::remove(path);
}

void aPathTooLongForSunPathIsRefused()
{
	const auto path = scratchDirectory() /
		std::string(sizeof(sockaddr_un::sun_path), 'n');
	::uml::device::SocketPath guard {path};
	requireThrows([&] { guard.adopt(); });
}

void anUnboundPathIsRefused()
{
	const auto path = scratchDirectory() / "never-bound.sock";
	std::filesystem::remove(path);
	::uml::device::SocketPath guard {path};
	requireThrows([&] { guard.adopt(); });
}

void theTableRefusesMoreThanItCanHold()
{
	std::vector<int> descriptors;
	std::vector<std::unique_ptr<::uml::device::SocketPath>> guards;
	bool refused = false;
	/* One more than the table holds, so the last adopt() must report the
	 * exhaustion instead of silently dropping a path. */
	for (int index = 0; index < 17; ++index) {
		const auto path = scratchDirectory() /
			("slot-" + std::to_string(index) + ".sock");
		std::filesystem::remove(path);
		descriptors.push_back(bindSocket(path));
		auto guard = std::make_unique<::uml::device::SocketPath>(path);
		try {
			guard->adopt();
		} catch (const std::exception &) {
			refused = true;
			break;
		}
		guards.push_back(std::move(guard));
	}
	require(refused);
	guards.clear();
	for (const int descriptor : descriptors)
		(void)::close(descriptor);
	for (int index = 0; index < 17; ++index)
		std::filesystem::remove(scratchDirectory() /
			("slot-" + std::to_string(index) + ".sock"));
}

} // namespace

int main()
{
	anAdoptedPathIsRemovedWhenTheGuardIsDestroyed();
	anUnadoptedPathIsLeftAlone();
	theSignalPathRemovesEveryAdoptedSocket();
	aReplacedPathSurvives();
	aPathTooLongForSunPathIsRefused();
	anUnboundPathIsRefused();
	theTableRefusesMoreThanItCanHold();
	std::filesystem::remove_all(scratchDirectory());
	return EXIT_SUCCESS;
}
