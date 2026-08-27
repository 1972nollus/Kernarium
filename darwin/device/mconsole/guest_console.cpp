// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_console.hpp"

#include "sockets/socket_paths.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace uml::device {

namespace {

/* arch/um/drivers/mconsole.h. The guest refuses anything else. */
constexpr std::uint32_t consoleMagic = 0xcafebabeU;
constexpr std::uint32_t consoleVersion = 2U;
constexpr std::size_t maximumConsoleData = 512U;

struct Request final {
	std::uint32_t magic;
	std::uint32_t version;
	std::uint32_t length;
	std::array<char, maximumConsoleData> data;
};

struct Reply final {
	std::uint32_t error;
	std::uint32_t more;
	std::uint32_t length;
	std::array<char, maximumConsoleData> data;
};

/* The guest kernel prints this once its filesystem writeback has begun. Its
 * absence means the request reached a console that refused to act on it. */
constexpr std::string_view writebackAcknowledgement = "Emergency Sync";
constexpr std::string_view suspendAcknowledgement = "suspended";
constexpr std::string_view stopAcknowledgement = "stopped";

std::mutex consoleMutex;
bool consoleAdopted;
std::unique_ptr<SocketPath> consoleGuard;
std::atomic_uint64_t nextClientId;

/* arch/um/os-Darwin/umid.c composes the path this way. */
[[nodiscard]] std::string consolePath()
{
	return "/tmp/uml-" + guest_management_console_umid() + "-mconsole";
}

[[nodiscard]] bool fillAddress(sockaddr_un &address, const std::string &path)
{
	if (path.size() >= sizeof(address.sun_path))
		return false;
	address = {};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
	return true;
}

/*
 * The reply comes back to the sender's address, so the client has to be bound.
 * Darwin does not autobind a Unix datagram socket, and the application's own
 * temporary directory is far longer than sun_path allows, so this shares the
 * short directory the guest's console already lives in.
 */
class Client final {
public:
	Client()
	{
		path_ = "/tmp/uml-mc-" + std::to_string(::getpid()) + "-" +
			std::to_string(nextClientId.fetch_add(
				1U, std::memory_order_relaxed));
		(void)::unlink(path_.c_str());
		descriptor_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
		if (descriptor_ < 0)
			return;
		sockaddr_un address {};
		if (!fillAddress(address, path_) ||
		    ::bind(descriptor_, reinterpret_cast<sockaddr *>(&address),
			    sizeof(address)) != 0) {
			::close(descriptor_);
			descriptor_ = -1;
		}
	}

	~Client()
	{
		if (descriptor_ >= 0)
			::close(descriptor_);
		(void)::unlink(path_.c_str());
	}

	Client(const Client &) = delete;
	Client &operator=(const Client &) = delete;

	[[nodiscard]] bool usable() const noexcept { return descriptor_ >= 0; }
	[[nodiscard]] int descriptor() const noexcept { return descriptor_; }

private:
	int descriptor_ {-1};
	std::string path_;
};

/*
 * The guest created this socket, so it can only be adopted once it exists.
 * Naming it after this process makes anything at that path ours; tightening
 * the mode keeps the guest's console away from other local users, because a
 * console accepts requests that stop the guest.
 */
void adoptConsolePath(const std::string &path)
{
	if (consoleAdopted)
		return;
	struct stat details {};
	if (::lstat(path.c_str(), &details) != 0 || !S_ISSOCK(details.st_mode))
		return;
	(void)::chmod(path.c_str(), S_IRUSR | S_IWUSR);
	try {
		auto guard = std::make_unique<SocketPath>(path);
		guard->adopt();
		consoleGuard = std::move(guard);
		consoleAdopted = true;
	} catch (...) {
		/* Cleanup on a terminating signal is a convenience; failing to
		 * arrange it must not fail the writeback request itself. */
	}
}

[[nodiscard]] bool exchange(
	int descriptor,
	const sockaddr_un &address,
	std::string_view command,
	std::string &response)
{
	Request request {consoleMagic, consoleVersion,
		static_cast<std::uint32_t>(command.size()), {}};
	if (command.size() >= request.data.size())
		return false;
	std::memcpy(request.data.data(), command.data(), command.size());
	const auto sent = ::sendto(descriptor, &request, sizeof(request), 0,
		reinterpret_cast<const sockaddr *>(&address), sizeof(address));
	if (sent != static_cast<ssize_t>(sizeof(request)))
		return false;
	for (;;) {
		Reply reply {};
		const auto received = ::recv(descriptor, &reply, sizeof(reply), 0);
		if (received < static_cast<ssize_t>(sizeof(reply) -
			    maximumConsoleData))
			return false;
		const auto length = std::min(
			static_cast<std::size_t>(reply.length),
			maximumConsoleData);
		response.append(reply.data.data(), length);
		if (reply.more == 0U)
			return reply.error == 0U;
	}
}

} // namespace

std::string guest_management_console_umid()
{
	/* One console per process. Two guests in one process cannot both own the
	 * kernel's single console anyway, and embedded runs are serialized. */
	static const std::string umid = "kern" + std::to_string(::getpid());
	return umid;
}

bool request_guest_writeback(unsigned int settleMs) noexcept
{
	try {
		std::lock_guard lock(consoleMutex);
		const auto path = consolePath();
		adoptConsolePath(path);
		sockaddr_un address {};
		if (!fillAddress(address, path))
			return false;
		Client client;
		if (!client.usable())
			return false;
		const timeval timeout {.tv_sec = 2, .tv_usec = 0};
		(void)::setsockopt(client.descriptor(), SOL_SOCKET, SO_RCVTIMEO,
			&timeout, sizeof(timeout));
		std::string response;
		if (!exchange(client.descriptor(), address, "sysrq s", response))
			return false;
		if (response.find(writebackAcknowledgement) == std::string::npos) {
			std::fputs(
				"Guest console refused the filesystem writeback; "
				"the guest has magic SysRq disabled\n",
				stderr);
			return false;
		}
		/* The guest reports that the writeback started, not that it
		 * finished, so give it a bounded chance to reach the block layer
		 * before the host flushes what it received. */
		if (settleMs != 0U) {
			std::this_thread::sleep_for(
				std::chrono::milliseconds(settleMs));
		}
		return true;
	} catch (...) {
		return false;
	}
}

GuestSuspension suspend_guest(unsigned int settleMs) noexcept
{
	try {
		std::lock_guard lock(consoleMutex);
		const auto path = consolePath();
		adoptConsolePath(path);
		sockaddr_un address {};
		if (!fillAddress(address, path))
			return GuestSuspension::none;
		const auto command = [&address](std::string_view request,
			std::string &response) {
			Client client;
			if (!client.usable())
				return false;
			const timeval timeout {.tv_sec = 2, .tv_usec = 0};
			(void)::setsockopt(client.descriptor(), SOL_SOCKET,
				SO_RCVTIMEO, &timeout, sizeof(timeout));
			return exchange(client.descriptor(), address, request,
				response);
		};
		std::string response;
		/* A suspend-capable guest replies only after devices, secondary
		 * CPUs, interrupts, and timekeeping have reached the platform sleep
		 * boundary. The separate wake request remains queued on the console
		 * descriptor until the kernel has resumed it. */
		if (command("suspend", response) &&
		    response.find(suspendAcknowledgement) != std::string::npos)
			return GuestSuspension::system;
		response.clear();
		/* Accepted older kernels have no suspend command. Preserve their
		 * stop/go behavior so a newer runtime can still quiesce them. */
		if (!command("stop", response) ||
		    response.find(stopAcknowledgement) == std::string::npos)
			return GuestSuspension::none;
		/* Block I/O already handed to the host keeps completing after the
		 * guest stops issuing more. Let it drain before the caller treats
		 * the disk as settled. */
		if (settleMs != 0U) {
			std::this_thread::sleep_for(
				std::chrono::milliseconds(settleMs));
		}
		return GuestSuspension::managementStop;
	} catch (...) {
		return GuestSuspension::none;
	}
}

void resume_guest(GuestSuspension suspension) noexcept
{
	if (suspension == GuestSuspension::none)
		return;
	try {
		std::lock_guard lock(consoleMutex);
		sockaddr_un address {};
		if (!fillAddress(address, consolePath()))
			return;
		Client client;
		if (!client.usable())
			return;
		const timeval timeout {.tv_sec = 2, .tv_usec = 0};
		(void)::setsockopt(client.descriptor(), SOL_SOCKET, SO_RCVTIMEO,
			&timeout, sizeof(timeout));
		std::string response;
		const auto command = suspension == GuestSuspension::system ?
			"wake" : "go";
		(void)exchange(client.descriptor(), address, command, response);
	} catch (...) {
	}
}

void release_guest_management_console() noexcept
{
	try {
		std::lock_guard lock(consoleMutex);
		if (consoleGuard) {
			consoleGuard.reset();
			consoleAdopted = false;
			return;
		}
		const auto path = consolePath();
		struct stat details {};
		if (::lstat(path.c_str(), &details) == 0 &&
		    S_ISSOCK(details.st_mode))
			(void)::unlink(path.c_str());
	} catch (...) {
	}
}

} // namespace uml::device
