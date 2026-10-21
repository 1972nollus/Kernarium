// SPDX-License-Identifier: GPL-2.0-or-later
/* The shared unit-test helpers that do more than assert. */

#include "require.hpp"
#include "wait_until.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using namespace std::chrono_literals;

using uml::testing::require;

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

} // namespace

int main()
{
	waitUntilReturnsWhatTheDeadlineLeavesTrue();
	return 0;
}
