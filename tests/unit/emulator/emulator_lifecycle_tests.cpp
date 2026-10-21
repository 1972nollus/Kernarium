// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The lifecycle calls reach the guest only through its management console, so
 * a datagram socket bound where the guest would bind its console sees exactly
 * what a guest would be asked, in order. The same socket orders the guest's
 * wake against the hibernation steps a test supplies: when the guest is woken,
 * its RAM and threads are already back.
 */
#include "uml/emulator.h"
#include "guest_console.hpp"
#include "guest_hibernation.hpp"
#include "require.hpp"
#include "testing.hpp"
#include "wait_until.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
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

	/* Called on the console's thread with each request, before its reply. */
	std::function<void(const std::string &)> onRequest;
	/* A guest without suspend support: it refuses "suspend" and takes the
	 * management stop instead. */
	bool refuseSuspend {false};

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
		if (onRequest)
			onRequest(command);
		std::string_view text = "running";
		std::uint32_t error = 0U;
		if (command == "suspend" && refuseSuspend) {
			text = "Suspend failed: refused by the test";
			error = 1U;
		} else if (command == "suspend") {
			text = "suspended";
		} else if (command == "stop") {
			text = "stopped";
		}
		if (command == "trim once")
			text = "trimmed 0 KiB in 0 ms, compacted in 40 ms, "
			       "released 0 KiB in 1 ms";
		else if (command.starts_with("trim "))
			text = std::string_view {command}.substr(5);
		Reply reply {error, 0U, static_cast<std::uint32_t>(text.size()), {}};
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

/* ---- Hibernating the guest's RAM around a quiesce. ---- */

/* What happened, in order, on every thread: hibernation steps and the
 * guest's console requests share it. */
class Log final {
public:
	void add(std::string entry)
	{
		const std::lock_guard lock {mutex_};
		entries_.push_back(std::move(entry));
	}

	[[nodiscard]] std::vector<std::string> take()
	{
		const std::lock_guard lock {mutex_};
		return std::exchange(entries_, {});
	}

private:
	std::mutex mutex_;
	std::vector<std::string> entries_;
};

Log hibernationLog;
std::atomic<int> restoreFailures {0};

struct Steps final : uml::runtime::HibernationSteps {
	bool restoreSucceeds {true};
	/* The write waits for the abort request that resume sends. */
	bool writeWaitsForAbort {false};
	std::atomic<bool> aborted {false};
	std::atomic<bool> writing {false};

	bool prepare() noexcept override
	{
		hibernationLog.add("prepare");
		return true;
	}
	bool hold(uml::runtime::HibernationReport &) noexcept override
	{
		hibernationLog.add("hold");
		return true;
	}
	bool write(uml::runtime::HibernationReport &) noexcept override
	{
		writing = true;
		if (writeWaitsForAbort) {
			const bool abortedInTime = uml::testing::waitUntil(
				[this] { return aborted.load(); },
				std::chrono::seconds {10},
				std::chrono::milliseconds {2});
			hibernationLog.add(
				abortedInTime ? "write:aborted" : "write:timeout");
		} else {
			hibernationLog.add("write");
		}
		return true;
	}
	bool restore(uml::runtime::HibernationReport &) noexcept override
	{
		hibernationLog.add("restore");
		return restoreSucceeds;
	}
	void release() noexcept override { hibernationLog.add("release"); }
	void set_abort(bool abort) noexcept override
	{
		if (abort) {
			hibernationLog.add("abort");
			aborted = true;
		} else {
			aborted = false;
		}
	}
};

void countRestoreFailure() noexcept
{
	hibernationLog.add("failure");
	++restoreFailures;
}

/* Installs `steps` on `emulator`; they stay owned by it, and the test keeps a
 * pointer to flip switches. */
Steps *installSteps(uml_emulator *emulator)
{
	auto steps = std::make_unique<Steps>();
	Steps *const raw = steps.get();
	uml::emulator::testing::use_hibernation(emulator, std::move(steps),
		&countRestoreFailure);
	return raw;
}

using Entries = std::vector<std::string>;

/* The wake is the first thing the guest can act on, so by then the RAM must be
 * whole and every thread released. */
void aHibernatedGuestIsWholeBeforeItIsWoken()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.onRequest = [](const std::string &command) {
		hibernationLog.add("console:" + command);
	};
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	require(hibernationLog.take() == Entries {"console:trim once",
		"console:suspend", "prepare", "hold", "write"},
		"a quiesce hibernates after the guest is asleep");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(hibernationLog.take() == Entries {"abort", "restore", "release",
		"console:wake"}, "the RAM is back and the threads run before the wake");
}

void aSecondQuiesceKeepsTheHibernation()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	require(console.requests(0U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}).empty());
	require(hibernationLog.take() == Entries {"prepare", "hold", "write"},
		"a guest that is already hibernated is not hibernated again");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	(void)hibernationLog.take();
}

void aGuestThatCouldNotSuspendIsNotHibernated()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.refuseSuspend = true;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	const auto requests = console.requests(4U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	});
	require(requests == Requests {"trim once", "suspend", "sysrq s", "stop"},
		"a refused suspend falls back to the management stop");
	require(hibernationLog.take().empty(),
		"only a system suspension is hibernated");
	(void)console.requests(1U, [&] { uml_emulator_resume(emulator.get()); });
}

void aGuestWithoutAConsoleIsNotHibernated()
{
	const auto emulator = makeEmulator();
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(uml_emulator_quiesce(emulator.get()) == 0);
	require(hibernationLog.take().empty(), "nothing suspended, nothing to do");
	uml_emulator_resume(emulator.get());
}

/* A failed restore must not wake anything: the guest would run on missing
 * memory. The failure handler ends the process in production. */
void aFailedRestoreWakesNothing()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.onRequest = [](const std::string &command) {
		hibernationLog.add("console:" + command);
	};
	Steps *const steps = installSteps(emulator.get());
	steps->restoreSucceeds = false;
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	(void)hibernationLog.take();
	require(console.requests(0U, [&] {
		uml_emulator_resume(emulator.get());
	}).empty(), "a guest whose RAM is not back is not woken");
	require(hibernationLog.take() == Entries {"abort", "restore", "failure"},
		"the failure is reported and nothing is released");
	require(restoreFailures.load() == 1, "once");
	/* The guest stays quiesced; a later resume tries again. */
	steps->restoreSucceeds = true;
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(hibernationLog.take() == Entries {"abort", "restore", "release",
		"console:wake"}, "and wakes it once the RAM is back");
}

/* A resume that arrives while the image is still being written does not wait
 * for the whole write: it asks the write to stop, then restores. */
void aResumeDuringTheWriteStopsTheWrite()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	Steps *const steps = installSteps(emulator.get());
	steps->writeWaitsForAbort = true;
	(void)hibernationLog.take();
	/* The guest answers the quiesce's two requests and the resume's wake. */
	Requests heard;
	std::thread quiescing {[&] {
		heard = console.requests(3U, [&] {
			require(uml_emulator_quiesce(emulator.get()) == 0);
		});
	}};
	require(uml::testing::waitUntil([&] { return steps->writing.load(); },
		std::chrono::seconds {5}, std::chrono::milliseconds {2}),
		"the write must start");
	uml_emulator_resume(emulator.get());
	quiescing.join();
	require(heard == Requests {"trim once", "suspend", "wake"});
	const auto entries = hibernationLog.take();
	require(entries == Entries {"prepare", "hold", "abort", "write:aborted",
		"restore", "release"} || entries == Entries {"prepare", "hold",
		"abort", "write:aborted", "abort", "restore", "release"},
		"the resume stops the write, then restores");
}

/* Threads that are held cannot see a stop, so a stop restores first. */
void aStopBringsTheRamBack()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	(void)hibernationLog.take();
	uml_emulator_request_stop(emulator.get());
	require(hibernationLog.take() == Entries {"abort", "restore", "release"},
		"a stop runs the threads that were held");
	(void)console.requests(1U, [&] { uml_emulator_resume(emulator.get()); });
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
	aHibernatedGuestIsWholeBeforeItIsWoken();
	aSecondQuiesceKeepsTheHibernation();
	aGuestThatCouldNotSuspendIsNotHibernated();
	aGuestWithoutAConsoleIsNotHibernated();
	aFailedRestoreWakesNothing();
	aResumeDuringTheWriteStopsTheWrite();
	aStopBringsTheRamBack();
	return EXIT_SUCCESS;
}
