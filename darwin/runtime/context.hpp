// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml_darwin_cpu.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>

namespace uml::runtime {

struct ThreadExecutionState;
struct ThreadStateStore;

using GuestEntry = int (*)(void *, void *, unsigned long, unsigned long,
	unsigned long *);

class Context final {
public:
	Context() noexcept;
	~Context();

	Context(const Context &) = delete;
	Context &operator=(const Context &) = delete;

	[[nodiscard]] bool begin_run() noexcept;
	void end_run() noexcept;
	void request_stop() noexcept;
	[[nodiscard]] bool stop_requested() const noexcept;
	[[nodiscard]] const std::atomic_bool *stop_token() const noexcept;
	using RunAdmissionHook = void (*)(void *) noexcept;
	void set_run_admission_hook_for_testing(
		RunAdmissionHook hook, void *opaque) noexcept;
	using RunRetirementHook = void (*)(void *) noexcept;
	void set_run_retirement_hook_for_testing(
		RunRetirementHook hook, void *opaque) noexcept;

	[[nodiscard]] int set_execution_engine(
		darwin_uml_execution_engine engine) noexcept;
	[[nodiscard]] darwin_uml_execution_engine execution_engine() const noexcept;
	[[nodiscard]] GuestEntry guest_entry() const noexcept;

	ThreadExecutionState &thread_execution_state();
	[[nodiscard]] std::size_t
	thread_execution_state_count_for_testing() const noexcept;

private:
	void retire_thread_execution_states() noexcept;

	std::atomic<darwin_uml_execution_engine> selected_engine_;
	std::atomic<GuestEntry> selected_entry_;
	std::atomic_bool stop_requested_{false};
	mutable std::mutex run_mutex_;
	bool running_{};
	RunAdmissionHook run_admission_hook_{};
	void *run_admission_hook_context_{};
	RunRetirementHook run_retirement_hook_{};
	void *run_retirement_hook_context_{};
	mutable std::mutex states_mutex_;
	std::shared_ptr<ThreadStateStore> state_store_;
};

class ActiveContextScope final {
public:
	explicit ActiveContextScope(Context &context);
	~ActiveContextScope();

	ActiveContextScope(const ActiveContextScope &) = delete;
	ActiveContextScope &operator=(const ActiveContextScope &) = delete;

private:
	Context *previous_;
};

class RunScope final {
public:
	explicit RunScope(Context &context) noexcept;
	~RunScope();

	RunScope(const RunScope &) = delete;
	RunScope &operator=(const RunScope &) = delete;

	[[nodiscard]] bool entered() const noexcept { return entered_; }

private:
	Context &context_;
	bool entered_;
};

/* A kernel-created vCPU thread has no public handle parameter. It may use a
 * genuinely sole active context as a fallback. Once runs overlap, their
 * unbound threads remain ambiguous until every overlapping scope retires,
 * even if only one scope is still active. Ambiguous callers receive nullptr
 * and fail closed rather than borrowing either handle's state. With no public
 * run, legacy C callers retain the process-local compatibility context. */
[[nodiscard]] Context *current_context() noexcept;

} // namespace uml::runtime
