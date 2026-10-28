// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The loop guard of an application that boots a guest again after it
 * restarted itself: a restart less than 15 s after its boot started is early,
 * five early restarts in a row end the loop, and a boot of 15 s or more
 * resets the count.
 */
#include "require.hpp"
#include "restart_loop_guard.hpp"

#include <chrono>
#include <cstdio>

namespace {

using namespace std::chrono_literals;
using uml::emulator::RestartLoopGuard;
using uml::testing::require;

const RestartLoopGuard::Clock::time_point epoch{};

/* Boots at `start` and restarts after `lasted`; returns the guard's answer
 * and leaves the clock at the restart. */
RestartLoopGuard::Clock::time_point restart_after(RestartLoopGuard &guard,
	RestartLoopGuard::Clock::time_point start,
	std::chrono::milliseconds lasted, bool expected_answer)
{
	guard.boot_started(start);
	const auto restart = start + lasted;
	require(guard.may_restart(restart) == expected_answer,
		"the guard answered another way than the policy says");
	return restart;
}

void test_the_policy_is_fifteen_seconds_and_five_restarts()
{
	/* Applications and their documentation quote these numbers. */
	require(uml::emulator::early_restart_window == 15s,
		"an early restart is not one under 15 s");
	require(uml::emulator::early_restart_limit == 5U,
		"the loop does not end on the fifth early restart");
	require(uml::emulator::early_restart_exit_status == 3,
		"the launchers' guard exit status is not 3");
}

void test_a_boot_that_lasts_the_window_is_not_early()
{
	RestartLoopGuard guard;
	for (int index = 0; index != 100; ++index)
		(void)restart_after(guard, epoch + std::chrono::seconds(index * 20),
			15s, true);
	require(guard.early_restarts() == 0U,
		"a boot of 15 s counted as early");
	(void)restart_after(guard, epoch, 14999ms, true);
	require(guard.early_restarts() == 1U,
		"a boot just under 15 s did not count as early");
}

void test_the_fifth_early_restart_in_a_row_ends_the_loop()
{
	RestartLoopGuard guard;
	auto now = epoch;
	for (unsigned restart = 1; restart != uml::emulator::early_restart_limit;
	     ++restart) {
		now = restart_after(guard, now, 2s, true);
		require(guard.early_restarts() == restart,
			"the count did not follow the early restarts");
	}
	(void)restart_after(guard, now, 2s, false);
	require(guard.early_restarts() == uml::emulator::early_restart_limit,
		"the ending restart was not counted");
}

void test_a_long_boot_resets_the_count()
{
	RestartLoopGuard guard;
	auto now = epoch;
	/* Four early restarts, a boot that lasted, then four more early ones:
	 * eight early restarts, never five in a row. */
	for (int round = 0; round != 3; ++round) {
		for (int early = 0; early != 4; ++early)
			now = restart_after(guard, now, 1s, true);
		now = restart_after(guard, now, 15s, true);
		require(guard.early_restarts() == 0U,
			"a boot of 15 s did not reset the count");
	}
}

void test_a_guest_that_never_boots_ends_after_five_boots()
{
	RestartLoopGuard guard;
	auto now = epoch;
	unsigned boots = 0;
	for (;;) {
		guard.boot_started(now);
		++boots;
		now += 3s;
		if (!guard.may_restart(now))
			break;
		require(boots < 100U, "a guest that restarts at once never ended");
	}
	require(boots == uml::emulator::early_restart_limit,
		"the loop ended after another number of boots than the limit");
}

void test_a_restart_with_no_time_at_all_is_early()
{
	RestartLoopGuard guard;
	guard.boot_started(epoch);
	require(guard.may_restart(epoch), "the first early restart was refused");
	require(guard.early_restarts() == 1U, "an instant restart was not early");
}

} // namespace

int main()
{
	test_the_policy_is_fifteen_seconds_and_five_restarts();
	test_a_boot_that_lasts_the_window_is_not_early();
	test_the_fifth_early_restart_in_a_row_ends_the_loop();
	test_a_long_boot_resets_the_count();
	test_a_guest_that_never_boots_ends_after_five_boots();
	test_a_restart_with_no_time_at_all_is_early();
	std::puts("restart loop guard tests passed");
	return 0;
}
