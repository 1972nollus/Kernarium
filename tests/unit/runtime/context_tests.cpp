// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/context.hpp"
#include "runtime/execution_state.hpp"
#include "uml_darwin_cpu.h"
#include "uml_darwin_guest.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

struct AdmissionInterleave final {
	std::mutex mutex;
	std::condition_variable changed;
	bool at_boundary{};
	bool stop_started{};
	bool stop_returned{};
	bool hook_timed_out{};
};

struct RetirementInterleave final {
	std::mutex mutex;
	std::condition_variable changed;
	bool at_boundary{};
	bool contender_started{};
	bool release_retirement{};
	bool contender_admitted{};
	bool contender_state_acquired{};
	bool release_contender{};
	bool hook_timed_out{};
};

void admission_hook(void *opaque) noexcept
{
	auto &interleave = *static_cast<AdmissionInterleave *>(opaque);
	std::unique_lock lock{interleave.mutex};
	interleave.at_boundary = true;
	interleave.changed.notify_all();
	if (!interleave.changed.wait_for(lock, 5s, [&] {
		return interleave.stop_started;
	}))
		interleave.hook_timed_out = true;
}

void retirement_hook(void *opaque) noexcept
{
	auto &interleave = *static_cast<RetirementInterleave *>(opaque);
	std::unique_lock lock{interleave.mutex};
	interleave.at_boundary = true;
	interleave.changed.notify_all();
	if (!interleave.changed.wait_for(lock, 5s, [&] {
		return interleave.contender_started;
	})) {
		interleave.hook_timed_out = true;
		return;
	}
	if (!interleave.changed.wait_for(lock, 5s, [&] {
		return interleave.release_retirement;
	}))
		interleave.hook_timed_out = true;
}

void second_admission_hook(void *opaque) noexcept
{
	auto &interleave = *static_cast<RetirementInterleave *>(opaque);
	std::lock_guard lock{interleave.mutex};
	interleave.contender_admitted = true;
	interleave.changed.notify_all();
}

void test_admitted_stop_is_not_lost()
{
	uml::runtime::Context context;
	AdmissionInterleave interleave;
	context.set_run_admission_hook_for_testing(
		admission_hook, &interleave);
	bool entered = false;
	bool observed_stop = false;
	std::thread runner{[&] {
		uml::runtime::RunScope run{context};
		entered = run.entered();
		std::unique_lock lock{interleave.mutex};
		(void)interleave.changed.wait_for(lock, 5s, [&] {
			return interleave.stop_returned;
		});
		observed_stop = context.stop_requested();
	}};

	bool reached_boundary = false;
	{
		std::unique_lock lock{interleave.mutex};
		reached_boundary = interleave.changed.wait_for(lock, 5s, [&] {
			return interleave.at_boundary;
		});
		if (!reached_boundary) {
			interleave.stop_started = true;
			interleave.stop_returned = true;
		}
	}
	interleave.changed.notify_all();

	std::thread stopper;
	if (reached_boundary) {
		stopper = std::thread{[&] {
			{
				std::lock_guard lock{interleave.mutex};
				interleave.stop_started = true;
			}
			interleave.changed.notify_all();
			context.request_stop();
			{
				std::lock_guard lock{interleave.mutex};
				interleave.stop_returned = true;
			}
			interleave.changed.notify_all();
		}};
	}
	if (stopper.joinable())
		stopper.join();
	runner.join();
	context.set_run_admission_hook_for_testing(nullptr, nullptr);

	require(reached_boundary && !interleave.hook_timed_out,
		"run admission boundary interleave timed out");
	require(entered, "run was not admitted");
	require(observed_stop,
		"stop request for an admitted run was lost");
	require(!context.stop_requested(),
		"completed run retained its stop token");
	context.request_stop();
	require(!context.stop_requested(),
		"idle stop request leaked into a later run");
}

void test_next_run_cannot_enter_during_state_retirement()
{
	uml::runtime::Context context;
	RetirementInterleave interleave;
	require(context.begin_run(), "first retirement run was not admitted");
	context.thread_execution_state().cpu.registers[0] = 0xfeed;
	require(context.thread_execution_state_count_for_testing() == 1,
		"first retirement run did not create execution state");
	context.set_run_retirement_hook_for_testing(
		retirement_hook, &interleave);
	context.set_run_admission_hook_for_testing(
		second_admission_hook, &interleave);

	std::thread finisher{[&] { context.end_run(); }};
	{
		std::unique_lock lock{interleave.mutex};
		require(interleave.changed.wait_for(lock, 5s, [&] {
			return interleave.at_boundary;
		}), "run retirement boundary interleave timed out");
	}
	std::thread contender{[&] {
		{
			std::lock_guard lock{interleave.mutex};
			interleave.contender_started = true;
		}
		interleave.changed.notify_all();
		if (!context.begin_run())
			return;
		(void)context.thread_execution_state();
		{
			std::unique_lock lock{interleave.mutex};
			interleave.contender_state_acquired = true;
			interleave.changed.notify_all();
			(void)interleave.changed.wait_for(lock, 5s, [&] {
				return interleave.release_contender;
			});
		}
	}};

	{
		std::unique_lock lock{interleave.mutex};
		require(interleave.changed.wait_for(lock, 5s, [&] {
			return interleave.contender_started;
		}), "next run did not reach the retirement interleave");
		require(!interleave.contender_admitted,
			"next run entered while prior state retirement was open");
		interleave.release_retirement = true;
	}
	interleave.changed.notify_all();
	finisher.join();
	{
		std::unique_lock lock{interleave.mutex};
		require(interleave.changed.wait_for(lock, 5s, [&] {
			return interleave.contender_state_acquired;
		}), "next run did not acquire execution state");
	}

	context.set_run_admission_hook_for_testing(nullptr, nullptr);
	context.set_run_retirement_hook_for_testing(nullptr, nullptr);
	require(!interleave.hook_timed_out,
		"run retirement boundary interleave timed out");
	require(interleave.contender_admitted &&
		interleave.contender_state_acquired,
		"next run was not admitted after retirement completed");
	require(context.thread_execution_state_count_for_testing() == 1,
		"prior run erased the next run's execution state");
	context.end_run();
	{
		std::lock_guard lock{interleave.mutex};
		interleave.release_contender = true;
	}
	interleave.changed.notify_all();
	contender.join();
	require(context.thread_execution_state_count_for_testing() == 0,
		"next run state was not retired");
}

std::size_t enter_available_engines(uml::runtime::Context &context)
{
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = 0x1000;
	unsigned long tls = 0;
	(void)darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), 1, &tls);
	registers.pc = 0x1000;
	(void)darwin_uml_interpreter_enter_guest(
		&registers, &fp, sizeof(fp), 1, &tls);
	if (darwin_uml_jit_available() != 0) {
		registers.pc = 0x1000;
		(void)darwin_uml_jit_enter_guest(
			&registers, &fp, sizeof(fp), 1, &tls);
	}
	const auto &state = context.thread_execution_state();
	return static_cast<std::size_t>(std::count_if(
		state.engines.begin(), state.engines.end(),
		[](const auto &engine) { return engine != nullptr; }));
}

void test_thread_churn_retires_engine_state()
{
	uml::runtime::Context context;
	uml::runtime::RunScope run{context};
	require(run.entered(), "thread-churn run was not admitted");
	uml::runtime::ActiveContextScope active{context};
	constexpr std::size_t thread_count = 32;
	for (std::size_t index = 0; index < thread_count; ++index) {
		std::size_t engines = 0;
		std::thread worker{[&] {
			engines = enter_available_engines(context);
		}};
		worker.join();
		require(engines >= 2,
			"thread did not enter the gadget and interpreter engines");
		require(context.thread_execution_state_count_for_testing() == 0,
			"exited host thread retained CPU, memory, or engine state");
	}
}

void test_reused_thread_starts_fresh()
{
	uml::runtime::Context context;
	constexpr std::uint64_t stale_value = 0xfeed'face'cafe'beefULL;
	{
		uml::runtime::RunScope run{context};
		require(run.entered(), "first reuse run was not admitted");
		uml::runtime::ActiveContextScope active{context};
		require(enter_available_engines(context) >= 2,
			"first reuse run did not create translated engines");
		auto &state = context.thread_execution_state();
		state.cpu.registers[0] = stale_value;
		require(context.thread_execution_state_count_for_testing() == 1,
			"first reuse run did not retain its active thread state");
	}
	require(context.thread_execution_state_count_for_testing() == 0,
		"run completion did not retire thread execution state");
	{
		uml::runtime::RunScope run{context};
		require(run.entered(), "second reuse run was not admitted");
		uml::runtime::ActiveContextScope active{context};
		const auto &state = context.thread_execution_state();
		require(state.cpu.registers[0] != stale_value,
			"reused host thread inherited stale CPU state");
		require(std::ranges::none_of(state.engines,
			[](const auto &engine) { return engine != nullptr; }),
			"reused host thread inherited stale translated engines");
	}
}

} // namespace

int main()
{
	try {
		test_admitted_stop_is_not_lost();
		test_next_run_cannot_enter_during_state_retirement();
		test_thread_churn_retires_engine_state();
		test_reused_thread_starts_fresh();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "runtime context test failed: %s\n", error.what());
		return EXIT_FAILURE;
	} catch (...) {
		std::fprintf(stderr, "runtime context test failed: unknown exception\n");
		return EXIT_FAILURE;
	}
}
