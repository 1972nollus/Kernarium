// SPDX-License-Identifier: GPL-2.0-or-later
#include "context.hpp"

#include "cpu/engine_factory.hpp"
#include "execution_state.hpp"
#include "uml_darwin_guest.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uml::runtime {

struct ThreadStateStore final {
	struct State final {
		std::uint64_t registration{};
		std::unique_ptr<ThreadExecutionState> execution;
	};

	struct Acquired final {
		ThreadExecutionState *execution{};
		std::uint64_t registration{};
	};

	[[nodiscard]] Acquired acquire(std::thread::id thread)
	{
		std::lock_guard lock{mutex};
		if (const auto position = states.find(thread);
		    position != states.end())
			return {position->second.execution.get(),
				position->second.registration};
		auto execution = std::make_unique<ThreadExecutionState>();
		const auto registration = next_registration++;
		auto *result = execution.get();
		states.emplace(thread,
			State{registration, std::move(execution)});
		return {result, registration};
	}

	void retire(std::thread::id thread,
		std::uint64_t registration) noexcept
	{
		std::lock_guard lock{mutex};
		const auto position = states.find(thread);
		if (position != states.end() &&
		    position->second.registration == registration)
			states.erase(position);
	}

	void retire_all() noexcept
	{
		std::lock_guard lock{mutex};
		states.clear();
	}

	[[nodiscard]] std::size_t size() const noexcept
	{
		std::lock_guard lock{mutex};
		return states.size();
	}

	mutable std::mutex mutex;
	std::unordered_map<std::thread::id, State> states;
	std::uint64_t next_registration{1};
};

namespace {

#if UML_DEFAULT_EXECUTION_ENGINE_JIT
constexpr auto platform_default = DARWIN_UML_EXECUTION_JIT;
#else
constexpr auto platform_default = DARWIN_UML_EXECUTION_GADGETS;
#endif

[[nodiscard]] constexpr GuestEntry entry_for(
	darwin_uml_execution_engine engine) noexcept
{
	switch (engine) {
	case DARWIN_UML_EXECUTION_JIT:
		return darwin_uml_jit_enter_guest;
	case DARWIN_UML_EXECUTION_INTERPRETER:
		return darwin_uml_interpreter_enter_guest;
	case DARWIN_UML_EXECUTION_GADGETS:
	case DARWIN_UML_EXECUTION_AUTO:
		return darwin_uml_static_enter_guest;
	}
	return darwin_uml_static_enter_guest;
}

[[nodiscard]] constexpr ::uml::cpu::EngineKind cpu_kind(
	darwin_uml_execution_engine engine) noexcept
{
	switch (engine) {
	case DARWIN_UML_EXECUTION_INTERPRETER:
		return ::uml::cpu::EngineKind::interpreter;
	case DARWIN_UML_EXECUTION_JIT:
		return ::uml::cpu::EngineKind::jit;
	case DARWIN_UML_EXECUTION_AUTO:
	case DARWIN_UML_EXECUTION_GADGETS:
		return ::uml::cpu::EngineKind::gadgets;
	}
	return ::uml::cpu::EngineKind::gadgets;
}

thread_local Context *thread_context;
std::mutex active_mutex;
struct ActiveContextEntry final {
	ActiveContextScope *scope;
	Context *context;
	bool unbound_fallback_allowed;
};
std::vector<ActiveContextEntry> active_contexts;

struct ThreadStateRegistration final {
	std::weak_ptr<ThreadStateStore> store;
	std::thread::id thread;
	std::uint64_t registration{};
};

class ThreadStateRegistrations final {
public:
	~ThreadStateRegistrations()
	{
		for (const auto &entry : entries_)
			if (auto store = entry.store.lock())
				store->retire(entry.thread, entry.registration);
	}

	void bind(const std::shared_ptr<ThreadStateStore> &store,
		std::uint64_t registration)
	{
		const std::weak_ptr<ThreadStateStore> candidate{store};
		for (auto &entry : entries_) {
			if (!entry.store.owner_before(candidate) &&
			    !candidate.owner_before(entry.store)) {
				entry.registration = registration;
				return;
			}
		}
		entries_.push_back({candidate, std::this_thread::get_id(),
			registration});
	}

private:
	std::vector<ThreadStateRegistration> entries_;
};

thread_local ThreadStateRegistrations thread_state_registrations;

[[nodiscard]] Context &legacy_context() noexcept
{
	static Context context;
	return context;
}

} // namespace

Context::Context() noexcept
	: selected_engine_{platform_default}, selected_entry_{entry_for(platform_default)}
{
}

Context::~Context() = default;

bool Context::begin_run() noexcept
{
	std::lock_guard lock{run_mutex_};
	if (running_)
		return false;
	stop_requested_.store(false, std::memory_order_release);
	running_ = true;
	if (run_admission_hook_ != nullptr)
		run_admission_hook_(run_admission_hook_context_);
	return true;
}

void Context::end_run() noexcept
{
	std::lock_guard lock{run_mutex_};
	if (run_retirement_hook_ != nullptr)
		run_retirement_hook_(run_retirement_hook_context_);
	retire_thread_execution_states();
	stop_requested_.store(false, std::memory_order_release);
	running_ = false;
}

void Context::request_stop() noexcept
{
	std::lock_guard lock{run_mutex_};
	if (running_)
		stop_requested_.store(true, std::memory_order_release);
}

bool Context::stop_requested() const noexcept
{
	return stop_requested_.load(std::memory_order_acquire);
}

const std::atomic_bool *Context::stop_token() const noexcept
{
	return &stop_requested_;
}

void Context::set_run_admission_hook_for_testing(
	RunAdmissionHook hook, void *opaque) noexcept
{
	std::lock_guard lock{run_mutex_};
	run_admission_hook_ = hook;
	run_admission_hook_context_ = opaque;
}

void Context::set_run_retirement_hook_for_testing(
	RunRetirementHook hook, void *opaque) noexcept
{
	std::lock_guard lock{run_mutex_};
	run_retirement_hook_ = hook;
	run_retirement_hook_context_ = opaque;
}

int Context::set_execution_engine(darwin_uml_execution_engine engine) noexcept
{
	if (engine == DARWIN_UML_EXECUTION_AUTO)
		engine = platform_default;
	if (engine != DARWIN_UML_EXECUTION_GADGETS &&
	    engine != DARWIN_UML_EXECUTION_JIT &&
	    engine != DARWIN_UML_EXECUTION_INTERPRETER)
		return -1;
	if (!::uml::cpu::cpu_engine_available(cpu_kind(engine)))
		return -1;
	selected_engine_.store(engine, std::memory_order_release);
	selected_entry_.store(entry_for(engine), std::memory_order_release);
	return 0;
}

darwin_uml_execution_engine Context::execution_engine() const noexcept
{
	return selected_engine_.load(std::memory_order_acquire);
}

GuestEntry Context::guest_entry() const noexcept
{
	return selected_entry_.load(std::memory_order_acquire);
}

ThreadExecutionState &Context::thread_execution_state()
{
	std::shared_ptr<ThreadStateStore> store;
	{
		std::lock_guard lock{states_mutex_};
		if (!state_store_)
			state_store_ = std::make_shared<ThreadStateStore>();
		store = state_store_;
	}
	const auto acquired = store->acquire(std::this_thread::get_id());
	thread_state_registrations.bind(store, acquired.registration);
	return *acquired.execution;
}

std::size_t Context::thread_execution_state_count_for_testing() const noexcept
{
	std::shared_ptr<ThreadStateStore> store;
	{
		std::lock_guard lock{states_mutex_};
		store = state_store_;
	}
	return store ? store->size() : 0;
}

void Context::retire_thread_execution_states() noexcept
{
	std::shared_ptr<ThreadStateStore> store;
	{
		std::lock_guard lock{states_mutex_};
		store = state_store_;
	}
	if (store)
		store->retire_all();
}

ActiveContextScope::ActiveContextScope(Context &context)
	: previous_{thread_context}
{
	std::lock_guard lock{active_mutex};
	const bool unbound_fallback_allowed = active_contexts.empty();
	if (!unbound_fallback_allowed)
		for (auto &entry : active_contexts)
			entry.unbound_fallback_allowed = false;
	active_contexts.push_back(
		{this, &context, unbound_fallback_allowed});
	thread_context = &context;
}

ActiveContextScope::~ActiveContextScope()
{
	{
		std::lock_guard lock{active_mutex};
		const auto position = std::find_if(
			active_contexts.rbegin(), active_contexts.rend(),
			[this](const ActiveContextEntry &entry) {
				return entry.scope == this;
			});
		if (position != active_contexts.rend())
			active_contexts.erase(std::next(position).base());
	}
	thread_context = previous_;
}

RunScope::RunScope(Context &context) noexcept
	: context_{context}, entered_{context.begin_run()}
{
}

RunScope::~RunScope()
{
	if (entered_)
		context_.end_run();
}

Context *current_context() noexcept
{
	if (thread_context != nullptr)
		return thread_context;
	{
		std::lock_guard lock{active_mutex};
		if (active_contexts.size() == 1 &&
		    active_contexts.front().unbound_fallback_allowed)
			return active_contexts.front().context;
		if (!active_contexts.empty())
			return nullptr;
	}
	return &legacy_context();
}

} // namespace uml::runtime
