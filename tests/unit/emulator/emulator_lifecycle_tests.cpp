// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The lifecycle calls reach the guest only through its management console, so
 * a datagram socket bound where the guest would bind its console sees exactly
 * what a guest would be asked, in order.
 */
#include "uml/emulator.h"
#include "guest_console.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using uml::testing::require;

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

/* The console directory goes in a private $TMPDIR, so that this test neither
 * races other runs over the shared one nor leaves anything in it. */
class PrivateTemporaryDirectory final {
public:
	PrivateTemporaryDirectory()
	{
		std::array<char, 1024> base {};
		const auto length = ::confstr(_CS_DARWIN_USER_TEMP_DIR,
			base.data(), base.size());
		require(length > 0U && length <= base.size());
		std::string pattern {base.data()};
		if (!pattern.ends_with('/'))
			pattern += '/';
		pattern += "uml-lifecycle-test.XXXXXX";
		require(::mkdtemp(pattern.data()) != nullptr);
		path_ = std::move(pattern);
		require(::setenv("TMPDIR", path_.c_str(), 1) == 0);
	}

	PrivateTemporaryDirectory(const PrivateTemporaryDirectory &) = delete;
	PrivateTemporaryDirectory &operator=(
		const PrivateTemporaryDirectory &) = delete;

	~PrivateTemporaryDirectory()
	{
		std::error_code ignored;
		std::filesystem::remove_all(path_, ignored);
	}

private:
	std::string path_;
};

/* Stands where the guest's console would be and answers every request the
 * way a guest that has the trim command would. */
class FakeGuestConsole final {
public:
	FakeGuestConsole()
	{
		const auto &location = ::uml::device::guest_console_location();
		require(location.has_value());
		path_ = location->console;
		(void)::unlink(path_.c_str());
		descriptor_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
		require(descriptor_ >= 0);
		sockaddr_un address {};
		address.sun_family = AF_UNIX;
		require(path_.size() < sizeof(address.sun_path));
		std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1U);
		require(::bind(descriptor_,
			reinterpret_cast<const sockaddr *>(&address),
			sizeof(address)) == 0);
	}

	FakeGuestConsole(const FakeGuestConsole &) = delete;
	FakeGuestConsole &operator=(const FakeGuestConsole &) = delete;

	~FakeGuestConsole()
	{
		(void)::close(descriptor_);
		(void)::unlink(path_.c_str());
	}

	/* Answers `count` requests in another thread while `operation` runs,
	 * and returns them in the order they arrived. */
	template<typename Operation>
	std::vector<std::string> requests(std::size_t count,
		Operation &&operation)
	{
		std::vector<std::string> received;
		std::thread guest {[this, count, &received] {
			for (std::size_t index = 0; index < count; ++index) {
				/* The operation sends at once; a missing request
				 * fails the comparison instead of waiting here. */
				if (!ready(10000))
					return;
				received.push_back(answer());
			}
		}};
		std::forward<Operation>(operation)();
		guest.join();
		/* Nothing else arrives either. */
		require(!ready(200));
		return received;
	}

private:
	[[nodiscard]] bool ready(int milliseconds) const
	{
		pollfd waiting {descriptor_, POLLIN, 0};
		int result;
		do {
			result = ::poll(&waiting, 1, milliseconds);
		} while (result < 0 && errno == EINTR);
		require(result >= 0);
		return result > 0;
	}

	std::string answer() const
	{
		Request request {};
		sockaddr_un client {};
		socklen_t clientLength = sizeof(client);
		const auto received = ::recvfrom(descriptor_, &request,
			sizeof(request), 0, reinterpret_cast<sockaddr *>(&client),
			&clientLength);
		require(received >= static_cast<ssize_t>(
			sizeof(request) - maximumConsoleData));
		const std::string command(request.data.data(),
			std::min<std::size_t>(request.length, maximumConsoleData));
		std::string_view text = "running";
		if (command == "suspend")
			text = "suspended";
		else if (command == "trim once")
			text = "trimmed 0 KiB in 0 ms, compacted in 40 ms, "
			       "released 0 KiB in 1 ms";
		else if (command.starts_with("trim "))
			text = std::string_view {command}.substr(5);
		Reply reply {0U, 0U, static_cast<std::uint32_t>(text.size()), {}};
		std::memcpy(reply.data.data(), text.data(), text.size());
		require(::sendto(descriptor_, &reply, sizeof(reply), 0,
			reinterpret_cast<const sockaddr *>(&client), clientLength) ==
			static_cast<ssize_t>(sizeof(reply)));
		return command;
	}

	std::string path_;
	int descriptor_ {-1};
};

using Emulator = std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)>;
using Requests = std::vector<std::string>;

Emulator makeEmulator()
{
	const uml_emulator_config config {
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
	};
	uml_emulator *created = nullptr;
	require(uml_emulator_create(&config, &created) == UML_EMULATOR_OK);
	return {created, uml_emulator_destroy};
}

/* A quiesced guest keeps what it holds, so it trims before it suspends. */
void quiesceTrimsBeforeItSuspends(uml_emulator *emulator,
	FakeGuestConsole &console)
{
	require(console.requests(2U, [emulator] {
		require(uml_emulator_quiesce(emulator) == 0);
	}) == Requests {"trim once", "suspend"});
}

/* Any request would wake a quiesced guest, so nothing reaches it until the
 * resume, which carries the trimming change after the wake. */
void aQuiescedGuestHearsOnlyTheResume(uml_emulator *emulator,
	FakeGuestConsole &console)
{
	require(console.requests(0U, [emulator] {
		require(uml_emulator_trim_page_cache(emulator) ==
			UML_EMULATOR_OK);
		require(uml_emulator_set_page_cache_trimming(emulator, 1) ==
			UML_EMULATOR_OK);
		require(uml_emulator_quiesce(emulator) == 0);
	}).empty());
	require(console.requests(2U, [emulator] {
		uml_emulator_resume(emulator);
	}) == Requests {"wake", "trim on"});
}

void aRunningGuestIsAskedAtOnce(uml_emulator *emulator,
	FakeGuestConsole &console)
{
	require(console.requests(1U, [emulator] {
		require(uml_emulator_set_page_cache_trimming(emulator, 0) ==
			UML_EMULATOR_OK);
	}) == Requests {"trim off"});
	require(console.requests(1U, [emulator] {
		require(uml_emulator_trim_page_cache(emulator) ==
			UML_EMULATOR_OK);
	}) == Requests {"trim once"});
	/* Resuming a running guest changes nothing it was told. */
	require(console.requests(0U, [emulator] {
		uml_emulator_resume(emulator);
	}).empty());
}

void withoutAConsoleNothingWaits(uml_emulator *emulator)
{
	require(uml_emulator_set_page_cache_trimming(emulator, 1) ==
		-UML_EMULATOR_UNSUPPORTED);
	require(uml_emulator_trim_page_cache(emulator) ==
		-UML_EMULATOR_UNSUPPORTED);
	require(uml_emulator_set_page_cache_trimming(nullptr, 1) ==
		-UML_EMULATOR_INVALID_ARGUMENT);
	require(uml_emulator_trim_page_cache(nullptr) ==
		-UML_EMULATOR_INVALID_ARGUMENT);
}

} // namespace

int main()
{
	const PrivateTemporaryDirectory temporary;
	const ::uml::device::GuestConsoleRun run {true};
	const auto emulator = makeEmulator();
	withoutAConsoleNothingWaits(emulator.get());
	FakeGuestConsole console;
	quiesceTrimsBeforeItSuspends(emulator.get(), console);
	aQuiescedGuestHearsOnlyTheResume(emulator.get(), console);
	aRunningGuestIsAskedAtOnce(emulator.get(), console);
	return EXIT_SUCCESS;
}
