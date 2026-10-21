// SPDX-License-Identifier: GPL-2.0-or-later
/* The shared unit-test helpers that do more than assert. */

#include "require.hpp"
#include "wait_until.hpp"
#include "watchdog.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

using uml::testing::require;

/* The exit status of a child whose assertion failed: it catches the SIGABRT
 * that `fail` raises, so that no crash report is written. */
constexpr int aborted = 99;

struct Outcome final {
	int status{};
	std::string output;
};

/* Runs `body` in a child and returns its exit status with everything it
 * wrote to stdout and stderr. Call it while no other thread exists. */
template<typename Body>
Outcome inChild(Body &&body)
{
	int pipes[2];
	require(::pipe(pipes) == 0, "cannot create a pipe");
	const pid_t child = ::fork();
	require(child >= 0, "cannot fork");
	if (child == 0) {
		::close(pipes[0]);
		::dup2(pipes[1], STDOUT_FILENO);
		::dup2(pipes[1], STDERR_FILENO);
		std::signal(SIGABRT, [](int) { ::_exit(aborted); });
		const int status = body();
		std::fflush(nullptr);
		::_exit(status);
	}
	::close(pipes[1]);
	Outcome outcome;
	char buffer[256];
	for (ssize_t count; (count = ::read(pipes[0], buffer, sizeof buffer)) > 0;)
		outcome.output.append(buffer, static_cast<std::size_t>(count));
	::close(pipes[0]);
	int status{};
	require(::waitpid(child, &status, 0) == child, "cannot wait for child");
	require(WIFEXITED(status), "the child did not exit");
	outcome.status = WEXITSTATUS(status);
	return outcome;
}

[[nodiscard]] bool mentions(const Outcome &outcome, const char *text)
{
	return outcome.output.find(text) != std::string::npos;
}

void waitUntilReturnsWhatTheDeadlineLeavesTrue()
{
	int calls = 0;
	require(uml::testing::waitUntil([&] { ++calls; return true; }, 10s),
		"a wait that holds returns at once");
	require(calls == 1, "a wait that holds is judged once");

	const auto started = std::chrono::steady_clock::now();
	require(!uml::testing::waitUntil([] { return false; }, 30ms),
		"a wait that never holds times out");
	require(std::chrono::steady_clock::now() - started >= 30ms,
		"a timeout waits out the whole limit");

	calls = 0;
	require(uml::testing::waitUntil([&] { return ++calls == 2; }, 0ms),
		"the condition is judged once more after the deadline");

	std::atomic<bool> flag{};
	std::thread setter{[&] {
		std::this_thread::sleep_for(20ms);
		flag.store(true);
	}};
	require(uml::testing::waitUntil([&] { return flag.load(); }, 10s, 0ms),
		"a spinning wait sees another thread's store");
	setter.join();
}

void aWatchdogOnlyFiresWhenTheGuardedCodeIsLate()
{
	const auto started = std::chrono::steady_clock::now();
	{
		uml::testing::Watchdog watchdog{"a quick call", 10s};
	}
	require(std::chrono::steady_clock::now() - started < 5s,
		"ending a watch does not wait out its limit");

	const auto late = inChild([] {
		uml::testing::Watchdog watchdog{"the late call", 100ms};
		std::this_thread::sleep_for(10s);
		return 0;
	});
	require(late.status == EXIT_FAILURE, "a late call ends the process");
	require(mentions(late, "FAIL: the late call did not return within 0.1 s"),
		"a late call is named");
}

} // namespace

int main()
{
	waitUntilReturnsWhatTheDeadlineLeavesTrue();
	aWatchdogOnlyFiresWhenTheGuardedCodeIsLate();
	return 0;
}
