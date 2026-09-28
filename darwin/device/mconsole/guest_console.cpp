// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_console.hpp"

#include "sockets/socket_paths.hpp"

#include <TargetConditionals.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

enum class ExchangeResult {
	notSent,
	pending,
	accepted,
	rejected,
};

/* The guest kernel prints this once its filesystem writeback has begun. Its
 * absence means the request reached a console that refused to act on it. */
constexpr std::string_view writebackAcknowledgement = "Emergency Sync";
constexpr std::string_view suspendAcknowledgement = "suspended";
constexpr std::string_view stopAcknowledgement = "stopped";

/* A reply socket's identifier is this many hexadecimal digits, so the widest
 * reply path is known before any request is sent. */
constexpr std::size_t clientIdDigits = 8U;
constexpr std::size_t socketPathLimit = sizeof(sockaddr_un {}.sun_path);

std::mutex consoleMutex;
bool consoleAdopted;
std::unique_ptr<SocketPath> consoleGuard;
std::atomic_uint32_t nextClientId;

/* One console per process. Two guests in one process cannot both own the
 * kernel's single console anyway, and embedded runs are serialized. */
[[nodiscard]] const std::string &consoleUmid()
{
	static const std::string umid = "kern" + std::to_string(::getpid());
	return umid;
}

[[nodiscard]] std::optional<std::string> privateConsoleDirectory()
{
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
	const char *directory = std::getenv("TMPDIR");
	return std::string {directory == nullptr ? "" : directory};
#else
	return std::nullopt;
#endif
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
 * Darwin does not autobind a Unix datagram socket, so this binds next to the
 * guest's console. Each request binds a new name, so a reply that arrives
 * after its request gave up finds no socket instead of the next request's. A
 * reply that has not arrived within two seconds is left pending.
 */
class Client final {
public:
	explicit Client(const GuestConsoleLocation &location)
	{
		std::array<char, clientIdDigits + 1U> id {};
		(void)std::snprintf(id.data(), id.size(), "%08x",
			static_cast<unsigned int>(nextClientId.fetch_add(
				1U, std::memory_order_relaxed)));
		path_ = location.client_prefix + id.data();
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
			return;
		}
		const timeval timeout {.tv_sec = 2, .tv_usec = 0};
		(void)::setsockopt(descriptor_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
			sizeof(timeout));
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

/* Sends one command from a client of its own and collects the reply. */
[[nodiscard]] ExchangeResult exchange(
	const GuestConsoleLocation &location,
	const sockaddr_un &address,
	std::string_view command,
	std::string &response)
{
	Client client {location};
	if (!client.usable())
		return ExchangeResult::notSent;
	const int descriptor = client.descriptor();
	Request request {consoleMagic, consoleVersion,
		static_cast<std::uint32_t>(command.size()), {}};
	if (command.size() >= request.data.size())
		return ExchangeResult::notSent;
	std::memcpy(request.data.data(), command.data(), command.size());
	const auto sent = ::sendto(descriptor, &request, sizeof(request), 0,
		reinterpret_cast<const sockaddr *>(&address), sizeof(address));
	if (sent != static_cast<ssize_t>(sizeof(request)))
		return ExchangeResult::notSent;
	for (;;) {
		Reply reply {};
		const auto received = ::recv(descriptor, &reply, sizeof(reply), 0);
		if (received < static_cast<ssize_t>(sizeof(reply) -
			    maximumConsoleData))
			return ExchangeResult::pending;
		const auto length = std::min(
			static_cast<std::size_t>(reply.length),
			maximumConsoleData);
		response.append(reply.data.data(), length);
		if (reply.more == 0U)
			return reply.error == 0U ? ExchangeResult::accepted :
				ExchangeResult::rejected;
	}
}

[[nodiscard]] bool requestWriteback(const GuestConsoleLocation &location,
	const sockaddr_un &address, unsigned int settleMs)
{
	std::string response;
	if (exchange(location, address, "sysrq s", response) !=
	    ExchangeResult::accepted)
		return false;
	if (response.find(writebackAcknowledgement) == std::string::npos) {
		std::fputs(
			"Guest console refused the filesystem writeback; "
			"the guest has magic SysRq disabled\n",
			stderr);
		return false;
	}
	/* The guest reports that the writeback started, not that it finished, so
	 * give it a bounded chance to reach the block layer before the host flushes
	 * what it received. */
	if (settleMs != 0U) {
		std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
	}
	return true;
}

} // namespace

std::optional<GuestConsoleLocation> compose_guest_console_location(
	std::optional<std::string_view> private_directory,
	std::string_view umid, pid_t pid)
{
	GuestConsoleLocation location;
	if (private_directory) {
		/* arch/um/os-Darwin/umid.c composes <uml_dir>/<name> after dropping
		 * the same trailing slashes. */
		auto directory = *private_directory;
		while (directory.size() > 1U && directory.ends_with('/'))
			directory.remove_suffix(1U);
		if (!directory.starts_with('/'))
			return std::nullopt;
		location.directory = directory;
		location.console = location.directory + "/mconsole";
		location.client_prefix = location.directory + "/mc-";
	} else {
		/* And /tmp/uml-<umid>-<name> without uml_dir=. */
		location.console = "/tmp/uml-" + std::string {umid} + "-mconsole";
		location.client_prefix =
			"/tmp/uml-mc-" + std::to_string(pid) + "-";
	}
	if (location.console.size() >= socketPathLimit ||
	    location.client_prefix.size() + clientIdDigits >= socketPathLimit)
		return std::nullopt;
	return location;
}

const std::optional<GuestConsoleLocation> &guest_console_location()
{
	static const std::optional<GuestConsoleLocation> location = [] {
		const auto directory = privateConsoleDirectory();
		auto composed = compose_guest_console_location(
			directory, consoleUmid(), ::getpid());
		if (!composed) {
			std::fprintf(stderr,
				"Guest management console: no console under \"%s\": "
				"it needs an absolute directory whose socket paths fit "
				"in %zu bytes; the host cannot reach the guest's console\n",
				directory.value_or("/tmp").c_str(), socketPathLimit);
		}
		return composed;
	}();
	return location;
}

std::vector<std::string> guest_management_console_arguments()
{
	std::vector<std::string> arguments {"umid=" + consoleUmid()};
	const auto &location = guest_console_location();
	if (location && !location->directory.empty())
		arguments.push_back("uml_dir=" + location->directory);
	return arguments;
}

bool request_guest_writeback(unsigned int settleMs) noexcept
{
	try {
		std::lock_guard lock(consoleMutex);
		const auto &location = guest_console_location();
		if (!location)
			return false;
		adoptConsolePath(location->console);
		sockaddr_un address {};
		if (!fillAddress(address, location->console))
			return false;
		return requestWriteback(*location, address, settleMs);
	} catch (...) {
		return false;
	}
}

GuestSuspension suspend_guest(unsigned int writebackSettleMs,
	unsigned int stopSettleMs) noexcept
{
	try {
		std::lock_guard lock(consoleMutex);
		const auto &location = guest_console_location();
		if (!location)
			return GuestSuspension::none;
		adoptConsolePath(location->console);
		sockaddr_un address {};
		if (!fillAddress(address, location->console))
			return GuestSuspension::none;
		std::string response;
		/* A suspend-capable guest replies only after devices, secondary
		 * CPUs, interrupts, and timekeeping have reached the platform sleep
		 * boundary. The separate wake request remains queued on the console
		 * descriptor until the kernel has resumed it. */
		const auto suspendResult =
			exchange(*location, address, "suspend", response);
		if (suspendResult == ExchangeResult::accepted) {
			if (response.find(suspendAcknowledgement) == std::string::npos) {
				std::fputs("Guest console returned an unexpected suspend reply\n",
					stderr);
			}
			return GuestSuspension::system;
		}
		/* Once the request was sent, a receive timeout is ambiguous: a slow
		 * guest may already be freezing devices and CPUs before it can reply.
		 * Queueing legacy stop behind that suspend would make the stop run only
		 * after wake and freeze the freshly resumed guest. Remember system mode
		 * so foregrounding sends the matching wake instead. */
		if (suspendResult == ExchangeResult::pending)
			return GuestSuspension::system;
		if (suspendResult == ExchangeResult::notSent)
			return GuestSuspension::none;
		response.clear();
		/* An explicit rejection identifies an older or suspend-disabled kernel.
		 * Its stop path does not sync filesystems, so preserve the old writeback
		 * step only on this compatibility path. A suspend-capable kernel performs
		 * the sync itself and avoids spending the iOS background budget twice. */
		(void)requestWriteback(*location, address, writebackSettleMs);
		const auto stopResult = exchange(*location, address, "stop", response);
		if (stopResult == ExchangeResult::notSent ||
		    stopResult == ExchangeResult::rejected ||
		    (stopResult == ExchangeResult::accepted &&
		     response.find(stopAcknowledgement) == std::string::npos))
			return GuestSuspension::none;
		/* Block I/O already handed to the host keeps completing after the
		 * guest stops issuing more. Let it drain before the caller treats
		 * the disk as settled. */
		if (stopSettleMs != 0U) {
			std::this_thread::sleep_for(
				std::chrono::milliseconds(stopSettleMs));
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
		const auto &location = guest_console_location();
		sockaddr_un address {};
		if (!location || !fillAddress(address, location->console))
			return;
		std::string response;
		const auto command = suspension == GuestSuspension::system ?
			"wake" : "go";
		(void)exchange(*location, address, command, response);
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
		const auto &location = guest_console_location();
		if (!location)
			return;
		struct stat details {};
		if (::lstat(location->console.c_str(), &details) == 0 &&
		    S_ISSOCK(details.st_mode))
			(void)::unlink(location->console.c_str());
	} catch (...) {
	}
}

} // namespace uml::device
