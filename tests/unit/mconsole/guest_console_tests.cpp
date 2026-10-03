// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_console.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

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

using uml::testing::require;

std::string consolePath()
{
	const auto &location = ::uml::device::guest_console_location();
	require(location.has_value());
	return location->console;
}

int bindServer(const std::string &path)
{
	(void)::unlink(path.c_str());
	const int descriptor = ::socket(AF_UNIX, SOCK_DGRAM, 0);
	require(descriptor >= 0);
	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	require(path.size() < sizeof(address.sun_path));
	std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
	require(::bind(descriptor, reinterpret_cast<const sockaddr *>(&address),
		sizeof(address)) == 0);
	return descriptor;
}

std::string receiveRequest(int descriptor, sockaddr_un &client,
	socklen_t &clientLength)
{
	Request request {};
	clientLength = sizeof(client);
	const auto received = ::recvfrom(descriptor, &request, sizeof(request), 0,
		reinterpret_cast<sockaddr *>(&client), &clientLength);
	require(received >= static_cast<ssize_t>(sizeof(request) -
		maximumConsoleData));
	const auto length = std::min(static_cast<std::size_t>(request.length),
		maximumConsoleData);
	return std::string(request.data.data(), length);
}

/* Waits up to `milliseconds` for a request; false when none arrives. */
bool awaitRequest(int descriptor, int milliseconds)
{
	pollfd waiting {descriptor, POLLIN, 0};
	int ready;
	do {
		ready = ::poll(&waiting, 1, milliseconds);
	} while (ready < 0 && errno == EINTR);
	require(ready >= 0);
	return ready > 0;
}

void observeRequests(int descriptor, std::vector<std::string> &requests)
{
	sockaddr_un client {};
	socklen_t clientLength {};
	/* suspend_guest sends its request at once. When it sends nothing, the
	 * observer gives up so that the test fails on the result instead of
	 * waiting here for ever. */
	if (!awaitRequest(descriptor, 10000))
		return;
	requests.push_back(receiveRequest(descriptor, client, clientLength));

	if (!awaitRequest(descriptor, 2500))
		return;

	requests.push_back(receiveRequest(descriptor, client, clientLength));
	constexpr std::string_view acknowledgement = "stopped";
	Reply reply {0U, 0U, static_cast<std::uint32_t>(acknowledgement.size()), {}};
	std::memcpy(reply.data.data(), acknowledgement.data(),
		acknowledgement.size());
	require(::sendto(descriptor, &reply, sizeof(reply), 0,
		reinterpret_cast<const sockaddr *>(&client), clientLength) ==
		static_cast<ssize_t>(sizeof(reply)));
}

void aSuspendTimeoutCannotQueueLegacyStop()
{
	const auto path = consolePath();
	const int server = bindServer(path);
	std::vector<std::string> requests;
	std::thread observer {observeRequests, server, std::ref(requests)};

	const auto suspension = ::uml::device::suspend_guest(0U, 0U);
	observer.join();
	require(suspension == ::uml::device::GuestSuspension::system);
	require(requests == std::vector<std::string> {"suspend"});

	::uml::device::release_guest_management_console();
	(void)::close(server);
	(void)::unlink(path.c_str());
}

/* The widest reply socket a location may name. */
std::size_t widestClientPath(const ::uml::device::GuestConsoleLocation &location)
{
	return location.client_prefix.size() + 8U;
}

void aSharedLocationKeepsTheGuestsTmpNames()
{
	const auto location = ::uml::device::compose_guest_console_location(
		std::nullopt, "kern42", 42);
	require(location.has_value());
	require(location->directory.empty());
	require(location->console == "/tmp/uml-kern42-mconsole");
	require(location->client_prefix == "/tmp/uml-mc-42-");

	/* Nothing on macOS moves the console out of /tmp, so the guest is told
	 * only its name. */
	const auto &process = ::uml::device::guest_console_location();
	require(process.has_value() && process->directory.empty());
	const auto arguments =
		::uml::device::guest_management_console_arguments();
	require(arguments.size() == 1U && arguments[0].starts_with("umid=kern"));
	require(process->console == "/tmp/uml-" + arguments[0].substr(5U) +
		"-mconsole");
}

void aPrivateDirectoryHoldsShortNamesThatFit()
{
	/* $TMPDIR of an iOS application: 89 bytes with its trailing slash. */
	const std::string container =
		"/private/var/mobile/Containers/Data/Application/"
		"00000000-0000-4000-8000-000000000000/tmp";
	const auto location = ::uml::device::compose_guest_console_location(
		container + "/", "kern42", 42);
	require(location.has_value());
	require(location->directory == container);
	require(location->console == container + "/mconsole");
	require(location->client_prefix == container + "/mc-");
	require(location->console.size() < sizeof(sockaddr_un {}.sun_path));
	require(widestClientPath(*location) < sizeof(sockaddr_un {}.sun_path));

	const auto slashes = ::uml::device::compose_guest_console_location(
		container + "///", "kern42", 42);
	require(slashes.has_value() && slashes->directory == container);
}

void aPrivateDirectoryThatDoesNotFitIsRefused()
{
	/* The longest directory whose widest reply socket still fits. */
	constexpr std::size_t limit = sizeof(sockaddr_un {}.sun_path);
	const std::size_t longest = limit - 1U - std::string_view {"/mc-"}.size() -
		8U;
	const auto fitting = "/" + std::string(longest - 1U, 'd');
	const auto fits = ::uml::device::compose_guest_console_location(
		fitting, "kern42", 42);
	require(fits.has_value() && widestClientPath(*fits) == limit - 1U);

	/* One byte more is refused outright: not truncated, and not moved to the
	 * shared /tmp where the guest would not bind. */
	require(!::uml::device::compose_guest_console_location(
		fitting + "d", "kern42", 42).has_value());
	require(!::uml::device::compose_guest_console_location(
		"/" + std::string(limit, 'd'), "kern42", 42).has_value());
}

void aPrivateDirectoryMustBeAbsolute()
{
	require(!::uml::device::compose_guest_console_location(
		std::string_view {}, "kern42", 42).has_value());
	require(!::uml::device::compose_guest_console_location(
		"tmp/", "kern42", 42).has_value());
}

} // namespace

int main(int argc, char **argv)
{
	const std::string_view selected = argc > 1 ? argv[1] : "";
	if (selected == "location") {
		aSharedLocationKeepsTheGuestsTmpNames();
		aPrivateDirectoryHoldsShortNamesThatFit();
		aPrivateDirectoryThatDoesNotFitIsRefused();
		aPrivateDirectoryMustBeAbsolute();
		return EXIT_SUCCESS;
	}
	if (selected == "suspend-timeout") {
		aSuspendTimeoutCannotQueueLegacyStop();
		return EXIT_SUCCESS;
	}
	return EXIT_FAILURE;
}
