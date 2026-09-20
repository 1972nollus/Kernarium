// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "execution_engine.hpp"
#include "memory/mmu/word_shared_mutex.hpp"
#include "platform/run_routing.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace uml::runtime {

struct ThreadExecutionState;
struct ThreadStateStore;

using GuestEntry = int (*)(void *, void *, unsigned long, unsigned long,
	unsigned long *);

class Context;

/* Pins one host thread's execution object and the Context memory domain that
 * backs its translated caches. Run transitions and Context destruction wait
 * for every outstanding access; request_stop() remains independent so a guest
 * entry holding this access can still be interrupted. */
class ThreadExecutionStateAccess final {
public:
	ThreadExecutionStateAccess(ThreadExecutionStateAccess &&) noexcept = default;
	ThreadExecutionStateAccess &operator=(
		ThreadExecutionStateAccess &&) noexcept = delete;

	ThreadExecutionStateAccess(const ThreadExecutionStateAccess &) = delete;
	ThreadExecutionStateAccess &operator=(
		const ThreadExecutionStateAccess &) = delete;

	[[nodiscard]] ThreadExecutionState *operator->() const noexcept
	{
		return execution_.get();
	}

	[[nodiscard]] ThreadExecutionState &operator*() const noexcept
	{
		return *execution_;
	}

private:
	friend class Context;

	ThreadExecutionStateAccess(
		std::shared_lock<memory::WordSharedMutex> lifecycle,
		std::shared_ptr<ThreadExecutionState> execution) noexcept
		: lifecycle_{std::move(lifecycle)}, execution_{std::move(execution)}
	{
	}

	std::shared_lock<memory::WordSharedMutex> lifecycle_;
	std::shared_ptr<ThreadExecutionState> execution_;
};

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
	[[nodiscard]] int stop_notification_fd() const noexcept;

	[[nodiscard]] int set_execution_engine(ExecutionEngine engine) noexcept;
	[[nodiscard]] ExecutionEngine execution_engine() const noexcept;
	void set_top_byte_ignore(bool enabled) noexcept;
	[[nodiscard]] bool top_byte_ignore() const noexcept;
	[[nodiscard]] GuestEntry guest_entry() const noexcept;
	[[nodiscard]] std::uint64_t memory_domain() const noexcept;

	[[nodiscard]] ThreadExecutionStateAccess thread_execution_state();

private:
	void retire_thread_execution_states() noexcept;

	std::atomic<ExecutionEngine> selected_engine_;
	std::atomic<GuestEntry> selected_entry_;
	std::atomic_bool top_byte_ignore_{false};
	std::atomic_uint64_t memory_domain_;
	std::atomic_bool stop_requested_{false};
	int stop_notification_read_{-1};
	int stop_notification_write_{-1};
	mutable std::mutex run_mutex_;
	bool running_{};
	mutable memory::WordSharedMutex execution_lifecycle_mutex_;
	mutable std::mutex states_mutex_;
	std::shared_ptr<ThreadStateStore> state_store_;
};

class ActiveContextScope final {
public:
	explicit ActiveContextScope(Context &context);
	struct DeferredPublication final {};
	explicit ActiveContextScope(Context &context, DeferredPublication);
	~ActiveContextScope();

	ActiveContextScope(const ActiveContextScope &) = delete;
	ActiveContextScope &operator=(const ActiveContextScope &) = delete;

	/* A deferred scope is invisible until commit.  commit() cannot allocate,
	 * which lets callers prepare every participating registry before any
	 * previous fallback is invalidated. */
	void commit() noexcept;

private:
	::uml::routing::ActiveRouteScope route_;
};

/* Composition-facing run route. The provider endpoint is opaque here so the
 * runtime and device modules continue to depend only on the neutral routing
 * foundation, never on each other. */
class ActiveRunRouteScope final {
public:
	struct DeferredPublication final {};

	ActiveRunRouteScope(Context &context, void *network,
		DeferredPublication) noexcept;
	~ActiveRunRouteScope();

	ActiveRunRouteScope(const ActiveRunRouteScope &) = delete;
	ActiveRunRouteScope &operator=(const ActiveRunRouteScope &) = delete;

	void commit() noexcept;

private:
	::uml::routing::ActiveRouteScope route_;
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
 * genuinely sole active context as a fallback. While runs overlap, unbound
 * callers receive nullptr and fail closed rather than borrowing either
 * handle's state. Ambiguity remains sticky for the surviving route until the
 * complete overlapping cohort retires. With no public run, legacy C callers
 * retain the process-local compatibility context. */
[[nodiscard]] Context *current_context() noexcept;

} // namespace uml::runtime
