// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_console.hpp"
#include "guest_console_test_support.hpp"
#include "require.hpp"
#include "socket_paths.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using uml::testing::awaitRequest;
using uml::testing::bindServer;
using uml::testing::receiveRequest;
using uml::testing::require;
using uml::testing::sendReply;

const ::uml::device::GuestConsoleLocation &processLocation()
{
	const auto &location = ::uml::device::guest_console_location();
	require(location.has_value());
	return *location;
}

std::string consolePath()
{
	return processLocation().console;
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

	/* Past the host's wait for a suspend reply, so that a stop sent after
	 * it gave up shows too. */
	if (!awaitRequest(descriptor, 9500))
		return;

	requests.push_back(receiveRequest(descriptor, client, clientLength));
	require(sendReply(descriptor, client, clientLength, 0U, "stopped"));
}

void aSuspendTimeoutCannotQueueLegacyStop()
{
	/* The run's directory exists before the guest binds its console. */
	const ::uml::device::GuestConsoleRun run {true};
	const auto path = consolePath();
	const int server = bindServer(path);
	std::vector<std::string> requests;
	std::thread observer {observeRequests, server, std::ref(requests)};

	const auto suspension = ::uml::device::suspend_guest(0U, 0U);
	observer.join();
	require(suspension == ::uml::device::GuestSuspension::system);
	require(requests == std::vector<std::string> {"suspend"});
	(void)::close(server);
}

/* Answers each request in turn with the next scripted reply, an error flag
 * and its text, the first one only after `firstDelay`, then records whatever
 * else arrives shortly after, unanswered, so that a request too many shows in
 * the list. A reply whose request gave up finds no socket, and its loss shows
 * in the requests that follow. */
void answerInOrder(int descriptor, std::chrono::milliseconds firstDelay,
	std::vector<std::pair<std::uint32_t, std::string>> answers,
	std::vector<std::string> &requests)
{
	sockaddr_un client {};
	socklen_t clientLength {};
	for (const auto &[error, answer] : answers) {
		if (!awaitRequest(descriptor, 10000))
			return;
		requests.push_back(receiveRequest(descriptor, client, clientLength));
		if (requests.size() == 1U)
			std::this_thread::sleep_for(firstDelay);
		(void)sendReply(descriptor, client, clientLength, error, answer);
	}
	while (awaitRequest(descriptor, 500))
		requests.push_back(receiveRequest(descriptor, client, clientLength));
}

/* What a suspend-capable guest replies when it gave a suspend up. */
const std::pair<std::uint32_t, std::string> failedSuspend {
	1U, "Suspend failed: -16"};
const std::vector<std::pair<std::uint32_t, std::string>> failThenStop {
	failedSuspend, {0U, "SysRq : Emergency Sync"}, {0U, "stopped"}};

/* A guest that gives a suspend up is stopped instead. */
void aFailedSuspendFallsBackToStop()
{
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	std::vector<std::string> requests;
	std::thread observer {answerInOrder, server,
		std::chrono::milliseconds {0}, failThenStop, std::ref(requests)};
	const auto suspension = ::uml::device::suspend_guest(0U, 0U);
	observer.join();
	require(suspension == ::uml::device::GuestSuspension::managementStop);
	require(requests ==
		std::vector<std::string> {"suspend", "sysrq s", "stop"});
	(void)::close(server);
}

/* The guest replies to a suspend only after syncing its filesystems, so a
 * failure can arrive seconds after the request; the guest still runs then and
 * is stopped instead. */
void aLateSuspendFailureFallsBackToStop()
{
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	std::vector<std::string> requests;
	std::thread observer {answerInOrder, server,
		std::chrono::milliseconds {3000}, failThenStop,
		std::ref(requests)};
	const auto suspension = ::uml::device::suspend_guest(0U, 0U);
	observer.join();
	require(suspension == ::uml::device::GuestSuspension::managementStop);
	require(requests ==
		std::vector<std::string> {"suspend", "sysrq s", "stop"});
	(void)::close(server);
}

/* A kernel without the suspend command is stopped at once. */
void aKernelWithoutSuspendIsStoppedAtOnce()
{
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	std::vector<std::string> requests;
	std::thread observer {answerInOrder, server,
		std::chrono::milliseconds {0},
		std::vector<std::pair<std::uint32_t, std::string>> {
			{1U, "System suspend is not configured"},
			{0U, "SysRq : Emergency Sync"}, {0U, "stopped"}},
		std::ref(requests)};
	const auto suspension = ::uml::device::suspend_guest(0U, 0U);
	observer.join();
	require(suspension == ::uml::device::GuestSuspension::managementStop);
	require(requests ==
		std::vector<std::string> {"suspend", "sysrq s", "stop"});
	(void)::close(server);
}

void answerOneRequest(int descriptor, std::string_view answer,
	std::vector<std::string> &requests)
{
	sockaddr_un client {};
	socklen_t clientLength {};
	if (!awaitRequest(descriptor, 10000))
		return;
	requests.push_back(receiveRequest(descriptor, client, clientLength));
	require(sendReply(descriptor, client, clientLength, 0U, answer));
}

/* The power key's wake is one request, whatever the host last asked for. */
void aWakeSendsOneWakeRequest()
{
	const ::uml::device::GuestConsoleRun run {true};
	const auto path = consolePath();
	/* No guest console yet: nothing answers and nothing waits. */
	require(!::uml::device::wake_guest());
	const int server = bindServer(path);
	std::vector<std::string> requests;
	std::thread observer {answerOneRequest, server, "running",
		std::ref(requests)};
	const bool answered = ::uml::device::wake_guest();
	observer.join();
	require(answered);
	require(requests == std::vector<std::string> {"wake"});
	require(!awaitRequest(server, 200));
	(void)::close(server);
}

/* Answers one request with `answer` and `error` after `delayMs`. A client
 * that stopped waiting has removed its socket, so that reply goes nowhere. */
void answerLater(int descriptor, std::string_view answer, std::uint32_t error,
	int delayMs, std::vector<std::string> &requests)
{
	sockaddr_un client {};
	socklen_t clientLength {};
	if (!awaitRequest(descriptor, 10000))
		return;
	requests.push_back(receiveRequest(descriptor, client, clientLength));
	std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
	(void)sendReply(descriptor, client, clientLength, error, answer);
}

::uml::device::GuestRequest answeredTrim(int server,
	const std::function<::uml::device::GuestRequest()> &request,
	std::string_view answer, std::uint32_t error, int delayMs,
	std::string_view expected)
{
	std::vector<std::string> requests;
	std::thread observer {answerLater, server, answer, error, delayMs,
		std::ref(requests)};
	const auto result = request();
	observer.join();
	require(requests == std::vector<std::string> {std::string {expected}});
	return result;
}

/* Turning trimming on or off is one request, which a kernel without the
 * command refuses. */
void trimmingIsSwitchedByOneRequest()
{
	using ::uml::device::GuestRequest;
	using ::uml::device::set_guest_page_cache_trim;
	const ::uml::device::GuestConsoleRun run {true};
	const auto path = consolePath();
	/* No guest console yet: nothing is sent and nothing waits. */
	require(set_guest_page_cache_trim(true) == GuestRequest::unreachable);
	const int server = bindServer(path);
	require(answeredTrim(server, [] { return set_guest_page_cache_trim(true); },
			"on", 0U, 0, "trim on") == GuestRequest::done);
	require(answeredTrim(server, [] { return set_guest_page_cache_trim(false); },
			"off", 0U, 0, "trim off") == GuestRequest::done);
	require(answeredTrim(server, [] { return set_guest_page_cache_trim(true); },
			"Unknown command", 1U, 0, "trim on") == GuestRequest::refused);
	require(!awaitRequest(server, 200));
	(void)::close(server);
}

/* A pass takes longer than an ordinary request may wait, and is waited for
 * up to its own bound and no longer. */
void aTrimPassIsWaitedForUpToItsBound()
{
	using ::uml::device::GuestRequest;
	using ::uml::device::trim_guest_page_cache;
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	require(answeredTrim(server, [] { return trim_guest_page_cache(8000U); },
			"trimmed 4096 KiB in 3 ms, compacted in 40 ms, "
			"released 4096 KiB in 2 ms",
			0U, 2500,
			"trim once") == GuestRequest::done);
	/* The reply comes 2.5 seconds late; the request gives up long
	 * before, and before an ordinary request's two seconds. */
	std::chrono::steady_clock::duration waited {};
	require(answeredTrim(server,
			[&waited] {
				const auto start = std::chrono::steady_clock::now();
				const auto result = trim_guest_page_cache(200U);
				waited = std::chrono::steady_clock::now() - start;
				return result;
			},
			"trimmed 0 KiB in 0 ms, compacted in 40 ms, "
			"released 0 KiB in 1 ms",
			0U, 2500,
			"trim once") == GuestRequest::unanswered);
	require(waited < std::chrono::milliseconds(1800));
	(void)::close(server);
}

/* The widest reply socket a location may name. */
std::size_t widestClientPath(const ::uml::device::GuestConsoleLocation &location)
{
	return location.client_prefix.size() + 8U;
}

void aDirectoryHoldsShortNamesThatFit()
{
	/* $TMPDIR of an iOS application: 89 bytes with its trailing slash. */
	const std::string container =
		"/private/var/mobile/Containers/Data/Application/"
		"00000000-0000-4000-8000-000000000000/tmp";
	const auto location =
		::uml::device::compose_guest_console_location(container + "/");
	require(location.has_value());
	require(location->directory == container);
	require(location->console == container + "/mconsole");
	require(location->client_prefix == container + "/mc-");
	require(location->console.size() < sizeof(sockaddr_un {}.sun_path));
	require(widestClientPath(*location) < sizeof(sockaddr_un {}.sun_path));

	const auto slashes =
		::uml::device::compose_guest_console_location(container + "///");
	require(slashes.has_value() && slashes->directory == container);
}

void aDirectoryThatDoesNotFitIsRefused()
{
	/* The longest directory whose widest reply socket still fits. */
	constexpr std::size_t limit = sizeof(sockaddr_un {}.sun_path);
	const std::size_t longest = limit - 1U - std::string_view {"/mc-"}.size() -
		8U;
	const auto fitting = "/" + std::string(longest - 1U, 'd');
	const auto fits = ::uml::device::compose_guest_console_location(fitting);
	require(fits.has_value() && widestClientPath(*fits) == limit - 1U);

	/* One byte more is refused outright: never truncated. */
	require(!::uml::device::compose_guest_console_location(fitting + "d")
			 .has_value());
	require(!::uml::device::compose_guest_console_location(
		"/" + std::string(limit, 'd')).has_value());
}

void aDirectoryMustBeAbsolute()
{
	require(!::uml::device::compose_guest_console_location(
		std::string_view {}).has_value());
	require(!::uml::device::compose_guest_console_location("tmp/")
			 .has_value());
}

void aRunDirectoryGoesInTheFirstParentThatFits()
{
	const std::vector<std::string> temporary {
		"/var/folders/yb/lx9rftq56156_v7rkkngd8bc0000gn/T/"};
	const auto location =
		::uml::device::compose_run_console_location(temporary, "kern42");
	require(location.has_value());
	require(location->directory ==
		"/var/folders/yb/lx9rftq56156_v7rkkngd8bc0000gn/T/uml-kern42");
	require(location->console == location->directory + "/mconsole");
	require(location->client_prefix == location->directory + "/mc-");

	/* Unset, relative and too-long parents are passed over, as the
	 * Simulator's application directory is. */
	const std::vector<std::string> fallbacks {
		"", "relative", "/" + std::string(100U, 'd'), "/tmp//"};
	const auto fallback =
		::uml::device::compose_run_console_location(fallbacks, "kern42");
	require(fallback.has_value() &&
		fallback->directory == "/tmp/uml-kern42");
	const std::vector<std::string> root {"/"};
	require(::uml::device::compose_run_console_location(root, "kern42")
			->directory == "/uml-kern42");
	const std::vector<std::string> none {"", "/" + std::string(100U, 'd')};
	require(!::uml::device::compose_run_console_location(none, "kern42")
			 .has_value());
}

void theProcessNamesItsOwnDirectory()
{
	const auto &process = processLocation();
	const auto umid = "kern" + std::to_string(::getpid());
	require(std::filesystem::path {process.directory}.filename() ==
		"uml-" + umid);
	/* $TMPDIR is the first choice whenever it is set and short enough. */
	const char *temporary = std::getenv("TMPDIR");
	if (temporary != nullptr && temporary[0] == '/' &&
	    std::string_view {temporary}.size() < 70U) {
		require(std::filesystem::path {process.directory}.parent_path() ==
			std::filesystem::path {std::string {temporary} + "/"}
				.lexically_normal().parent_path());
	}
	const auto arguments =
		::uml::device::guest_management_console_arguments();
	require(arguments == std::vector<std::string> {
		"umid=" + umid, "uml_dir=" + process.directory});
}

bool directoryExists(const std::string &path)
{
	struct stat details {};
	return ::lstat(path.c_str(), &details) == 0 && S_ISDIR(details.st_mode);
}

bool socketExists(const std::string &path)
{
	struct stat details {};
	return ::lstat(path.c_str(), &details) == 0 &&
		S_ISSOCK(details.st_mode);
}

void aRunOwnsItsDirectoryUntilItReturns()
{
	const auto &location = processLocation();
	{
		const ::uml::device::GuestConsoleRun inactive {false};
		require(!directoryExists(location.directory));
	}
	int console = -1;
	{
		const ::uml::device::GuestConsoleRun run {true};
		struct stat details {};
		require(::lstat(location.directory.c_str(), &details) == 0 &&
			S_ISDIR(details.st_mode) && details.st_uid == ::geteuid() &&
			(details.st_mode & 07777) == S_IRWXU);
		/* The guest binds its console there, and a run killed before the
		 * guest shut down keeps it. */
		console = bindServer(location.console);
	}
	require(!directoryExists(location.directory));
	(void)::close(console);
}

void theTerminationPathRemovesTheRunDirectory()
{
	const auto &location = processLocation();
	const ::uml::device::GuestConsoleRun run {true};
	const int console = bindServer(location.console);
	/* What darwin_uml_exit, darwin_uml_abort and the host's signal
	 * handlers run; the guest's SIGINT and SIGTERM handler exits through
	 * darwin_uml_exit. */
	::uml::device::remove_registered_socket_paths();
	require(!directoryExists(location.directory));
	(void)::close(console);
}

void aDirectoryLeftByThisPidIsReplaced()
{
	const auto &location = processLocation();
	require(::mkdir(location.directory.c_str(), S_IRWXU) == 0);
	const int stale = bindServer(location.console);
	{
		const ::uml::device::GuestConsoleRun run {true};
		require(directoryExists(location.directory) &&
			!socketExists(location.console));
	}
	require(!directoryExists(location.directory));
	(void)::close(stale);
}

/* A pid whose process has exited and been reaped. */
pid_t exitedPid()
{
	const pid_t child = ::fork();
	require(child >= 0);
	if (child == 0)
		::_exit(0);
	int status = 0;
	require(::waitpid(child, &status, 0) == child);
	return child;
}

void createFile(const std::filesystem::path &path)
{
	require(::close(::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY,
		S_IRUSR | S_IWUSR)) == 0);
}

void aRunRemovesOnlyWhatExitedRunsLeft()
{
	const auto parent =
		std::filesystem::path {processLocation().directory}.parent_path();
	const auto dead = std::to_string(exitedPid());
	const auto live = std::to_string(::getppid());
	const auto abandoned = parent / ("uml-kern" + dead);
	const auto liveRun = parent / ("uml-kern" + live);
	const auto padded = parent / ("uml-kern0" + dead);
	const auto file = parent / ("uml-kern" + std::to_string(exitedPid()));
	/* Older builds bound the console in /tmp itself, the path under test. */
	const std::filesystem::path legacyDirectory {"/tmp"};
	const auto legacy = legacyDirectory / ("uml-kern" + dead + "-mconsole");
	const auto liveLegacy =
		legacyDirectory / ("uml-kern" + live + "-mconsole");
	const auto legacyFile = legacyDirectory /
		("uml-kern" + std::to_string(exitedPid()) + "-mconsole");
	const auto misplacedLegacy = parent / ("uml-kern" + dead + "-mconsole");
	for (const auto &directory : {abandoned, liveRun, padded})
		require(std::filesystem::create_directory(directory));
	const int abandonedConsole = bindServer(abandoned / "mconsole");
	const int legacyConsole = bindServer(legacy);
	const int liveLegacyConsole = bindServer(liveLegacy);
	const int misplacedConsole = bindServer(misplacedLegacy);
	createFile(file);
	createFile(legacyFile);

	{
		const ::uml::device::GuestConsoleRun run {true};
	}
	/* Only an exited process's run directory, and its loose console socket
	 * in /tmp, go. A live run's entries, another program's names, anything
	 * of another file type and a console name outside /tmp stay. */
	require(!std::filesystem::exists(abandoned));
	require(!std::filesystem::exists(legacy));
	require(directoryExists(liveRun) && directoryExists(padded) &&
		std::filesystem::is_regular_file(file));
	require(socketExists(liveLegacy) && socketExists(misplacedLegacy) &&
		std::filesystem::is_regular_file(legacyFile));

	for (const int descriptor : {abandonedConsole, legacyConsole,
		     liveLegacyConsole, misplacedConsole})
		(void)::close(descriptor);
	for (const auto &path :
		{liveRun, padded, file, liveLegacy, legacyFile, misplacedLegacy})
		std::filesystem::remove_all(path);
}

/* Runs a group under a private $TMPDIR, which must hold this process's run
 * directory. */
void runPrivately(void (*group)())
{
	const uml::testing::PrivateTemporaryDirectory temporary {
		"uml-console-test.XXXXXX"};
	require(std::filesystem::path {processLocation().directory}
			.parent_path() == temporary.path());
	group();
}

} // namespace

int main(int argc, char **argv)
{
	const std::string_view selected = argc > 1 ? argv[1] : "";
	if (selected == "location") {
		aDirectoryHoldsShortNamesThatFit();
		aDirectoryThatDoesNotFitIsRefused();
		aDirectoryMustBeAbsolute();
		aRunDirectoryGoesInTheFirstParentThatFits();
		theProcessNamesItsOwnDirectory();
	} else if (selected == "run") {
		runPrivately([] {
			aRunOwnsItsDirectoryUntilItReturns();
			theTerminationPathRemovesTheRunDirectory();
			aDirectoryLeftByThisPidIsReplaced();
			aRunRemovesOnlyWhatExitedRunsLeft();
		});
	} else if (selected == "wake") {
		runPrivately(aWakeSendsOneWakeRequest);
	} else if (selected == "suspend-failure") {
		runPrivately([] {
			aFailedSuspendFallsBackToStop();
			aLateSuspendFailureFallsBackToStop();
			aKernelWithoutSuspendIsStoppedAtOnce();
		});
	} else if (selected == "trim") {
		runPrivately([] {
			trimmingIsSwitchedByOneRequest();
			aTrimPassIsWaitedForUpToItsBound();
		});
	} else if (selected == "suspend-timeout") {
		runPrivately(aSuspendTimeoutCannotQueueLegacyStop);
	} else {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
