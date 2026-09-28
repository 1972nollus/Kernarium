// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_console.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
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

void require(bool condition)
{
	if (!condition)
		std::abort();
}

std::string consolePath()
{
	return "/tmp/uml-" + ::uml::device::guest_management_console_umid() +
		"-mconsole";
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

} // namespace

int main()
{
	aSuspendTimeoutCannotQueueLegacyStop();
	return EXIT_SUCCESS;
}
