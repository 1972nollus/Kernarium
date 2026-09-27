// SPDX-License-Identifier: GPL-2.0-or-later
#include "context.hpp"

#include "cpu/engine_factory.hpp"
#include "execution_state.hpp"
#include "memory/domain_contract.hpp"
#include "memory/process_singleton.hpp"
#include "platform/host_topology.hpp"
#include "uml_darwin_guest.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace uml::runtime {

struct ThreadStateStore final {
	struct State final {
		std::uint64_t registration{};
		std::shared_ptr<ThreadExecutionState> execution;
	};

	struct Acquired final {
		std::shared_ptr<ThreadExecutionState> execution;
		std::uint64_t registration{};
	};

	[[nodiscard]] Acquired acquire(std::thread::id thread)
	{
		std::lock_guard lock{mutex};
		if (const auto position = states.find(thread);
		    position != states.end())
			return {position->second.execution,
				position->second.registration};
		auto execution = std::make_shared<ThreadExecutionState>();
		const auto registration = next_registration++;
		auto result = execution;
		states.emplace(thread,
			State{registration, std::move(execution)});
		return {std::move(result), registration};
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

	mutable std::mutex mutex;
	std::unordered_map<std::thread::id, State> states;
	std::uint64_t next_registration{1};
};

namespace {

#if UML_DEFAULT_EXECUTION_ENGINE_JIT
constexpr auto platform_default = ExecutionEngine::jit;
#else
constexpr auto platform_default = ExecutionEngine::gadgets;
#endif

[[nodiscard]] constexpr GuestEntry entry_for(ExecutionEngine engine) noexcept
{
	switch (engine) {
	case ExecutionEngine::jit:
		return darwin_uml_jit_enter_guest;
	case ExecutionEngine::gadgets:
	case ExecutionEngine::automatic:
		return darwin_uml_static_enter_guest;
	}
	return nullptr;
}

[[nodiscard]] constexpr std::optional<::uml::cpu::EngineKind> cpu_kind(
	ExecutionEngine engine) noexcept
{
	switch (engine) {
	case ExecutionEngine::jit:
		return ::uml::cpu::EngineKind::jit;
	case ExecutionEngine::automatic:
		return std::nullopt;
	case ExecutionEngine::gadgets:
		return ::uml::cpu::EngineKind::gadgets;
	}
	return std::nullopt;
}

std::atomic_uint64_t next_memory_domain{1};

[[nodiscard]] std::uint64_t fresh_memory_domain() noexcept
{
	for (;;) {
		const auto domain = next_memory_domain.fetch_add(
			1, std::memory_order_relaxed) + 1;
		if (domain > 1)
			return domain;
	}
}

[[nodiscard]] bool provide_memory_domain(std::uint64_t &domain) noexcept;

struct DomainProviderRegistration final {
	DomainProviderRegistration() noexcept
	{
		::uml::memory::set_domain_provider(&provide_memory_domain);
	}
};
const DomainProviderRegistration domain_provider_registration;

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
		entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
			[](const ThreadStateRegistration &entry) {
				return entry.store.expired();
			}), entries_.end());
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

/* Every guest entry resolves its execution object twice, once from
 * darwin_uml_static_guest_interrupt_sequence() and once from run_common_guest(),
 * and the answer is per-thread and does not change between retirements. Each
 * resolution took a shared_lock on execution_lifecycle_mutex_, a lock_guard on
 * states_mutex_ and a shared_ptr copy, then a third mutex plus a
 * thread::id hash lookup and a second shared_ptr copy inside
 * ThreadStateStore::acquire(), then a vector scan in bind() whose predicate
 * dereferences a weak_ptr control block per entry. On a syscall-heavy workload
 * that is six mutex acquisitions and two hash lookups per syscall, on
 * process-global cachelines shared by every vCPU thread.
 *
 * The resolved object is therefore cached per thread, and the entry is weak
 * rather than owning. That is what makes it self-invalidating, and it is the
 * whole correctness argument: retire_thread_execution_states() clears the store,
 * which drops the only strong reference - retirement runs under
 * execution_lifecycle_mutex_ held exclusively, so no ThreadExecutionStateAccess
 * can be outstanding to hold another - the object is destroyed, and lock() then
 * fails and sends this thread down the slow path. An explicit validity epoch was
 * written first and then removed: it could not be made to fail any test, because
 * the only state it guarded against is one no caller can produce.
 *
 * Weak also preserves destruction order. ThreadExecutionState carries the engine
 * slots and the guest memory view, and retirement destroys it before the memory
 * domain behind those caches is retired. An owning cache would defer that
 * destructor to this thread's next resolution or to thread exit, moving it after
 * the domain retirement it is ordered before.
 *
 * The Context pointer is still needed: two live Contexts resolve different
 * objects for the same thread, and a live entry from one must not answer for the
 * other. */
struct ResolvedThreadExecution final {
	const Context *context{};
	std::weak_ptr<ThreadExecutionState> execution;
};

thread_local ResolvedThreadExecution resolved_thread_execution;

std::atomic<Context *> published_legacy_context{};
std::atomic_flag legacy_context_initialization = ATOMIC_FLAG_INIT;

[[nodiscard]] Context *legacy_context() noexcept
{
	/* The compatibility context is process state, not an emulator handle.  Its
	 * destructor would otherwise run after this thread's MMU TLS destructors on
	 * Darwin and attempt to retire through already-destroyed thread-local
	 * owners.  Deliberately give it process lifetime, just like the MMU domain
	 * registry it addresses. A transient first allocation failure must remain
	 * retryable rather than permanently caching an unavailable compatibility
	 * context. */
	return uml::memory::process_singleton(published_legacy_context,
		legacy_context_initialization,
		[]() noexcept { return new (std::nothrow) Context; });
}

} // namespace

Context::Context() noexcept
	: selected_engine_{platform_default}, selected_entry_{entry_for(platform_default)},
	  memory_domain_{fresh_memory_domain()}
{
	int descriptors[2];

	if (pipe(descriptors) != 0)
		return;
	const auto configure = [](int descriptor) {
		const int status = fcntl(descriptor, F_GETFL);
		return status >= 0 &&
			fcntl(descriptor, F_SETFL, status | O_NONBLOCK) == 0 &&
			fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0;
	};
	if (!configure(descriptors[0]) || !configure(descriptors[1])) {
		close(descriptors[0]);
		close(descriptors[1]);
		return;
	}
	stop_notification_read_ = descriptors[0];
	stop_notification_write_ = descriptors[1];
}

Context::~Context()
{
	std::unique_lock lifecycle_lock{execution_lifecycle_mutex_};
	retire_thread_execution_states();
	::uml::memory::retire_domain(
		memory_domain_.exchange(0, std::memory_order_acq_rel));
	if (stop_notification_read_ >= 0)
		close(stop_notification_read_);
	if (stop_notification_write_ >= 0)
		close(stop_notification_write_);
}

bool Context::begin_run() noexcept
{
	std::lock_guard lock{run_mutex_};
	if (running_ || stop_notification_read_ < 0 ||
	    stop_notification_write_ < 0)
		return false;
	std::unique_lock lifecycle_lock{execution_lifecycle_mutex_};
	char pending[64];
	while (read(stop_notification_read_, pending, sizeof(pending)) > 0)
		;
	stop_requested_.store(false, std::memory_order_release);
	/* A context can be routed through the compatibility-facing internal scope
	 * before its first run. Retire every execution object while its old memory
	 * domain is still valid, then rotate and retire that domain before admitting
	 * the run. thread_execution_state() takes the same lifecycle lock, so a
	 * concurrent pre-run acquisition cannot publish between these two steps.
	 * Later runs normally exchange zero because end_run() already retired their
	 * domain. All operations in this boundary are allocation-free. */
	retire_thread_execution_states();
	const auto previous_domain = memory_domain_.exchange(
		fresh_memory_domain(), std::memory_order_acq_rel);
	::uml::memory::retire_domain(previous_domain);
	running_ = true;
	return true;
}

void Context::end_run() noexcept
{
	std::lock_guard lock{run_mutex_};
	std::unique_lock lifecycle_lock{execution_lifecycle_mutex_};
	retire_thread_execution_states();
	::uml::memory::retire_domain(
		memory_domain_.exchange(0, std::memory_order_acq_rel));
	stop_requested_.store(false, std::memory_order_release);
	running_ = false;
}

void Context::request_stop() noexcept
{
	std::lock_guard lock{run_mutex_};
	if (!running_ ||
	    stop_requested_.exchange(true, std::memory_order_acq_rel))
		return;
	const char wake = 1;
	ssize_t written;
	do {
		written = write(stop_notification_write_, &wake, sizeof(wake));
	} while (written < 0 && errno == EINTR);
	/* EAGAIN means an earlier byte already makes the descriptor readable. */
}

bool Context::stop_requested() const noexcept
{
	return stop_requested_.load(std::memory_order_acquire);
}

const std::atomic_bool *Context::stop_token() const noexcept
{
	return &stop_requested_;
}

int Context::stop_notification_fd() const noexcept
{
	return stop_notification_read_;
}

namespace {

/* Resolve the automatic selection to a kind this build can actually run.
 *
 * The platform default states a preference, not a guarantee: a build that
 * prefers the JIT still has to cope with a host that cannot publish into a
 * MAP_JIT mapping. Falling back to the always-available gadget engine keeps
 * "automatic" from being the one selection that can fail outright.
 */
[[nodiscard]] ExecutionEngine resolve_automatic() noexcept
{
	const auto kind = cpu_kind(platform_default);
	if (kind.has_value() && ::uml::cpu::cpu_engine_available(*kind))
		return platform_default;
	return ExecutionEngine::gadgets;
}

} // namespace

bool execution_engine_available(ExecutionEngine engine) noexcept
{
	if (engine == ExecutionEngine::automatic)
		engine = resolve_automatic();
	const auto kind = cpu_kind(engine);
	return kind.has_value() && ::uml::cpu::cpu_engine_available(*kind);
}

int Context::set_execution_engine(ExecutionEngine engine) noexcept
{
	if (engine == ExecutionEngine::automatic)
		engine = resolve_automatic();
	if (engine != ExecutionEngine::gadgets &&
	    engine != ExecutionEngine::jit)
		return -1;
	if (!execution_engine_available(engine))
		return -1;
	selected_engine_.store(engine, std::memory_order_release);
	selected_entry_.store(entry_for(engine), std::memory_order_release);
	return 0;
}

ExecutionEngine Context::execution_engine() const noexcept
{
	return selected_engine_.load(std::memory_order_acquire);
}

void Context::set_top_byte_ignore(bool enabled) noexcept
{
	top_byte_ignore_.store(enabled, std::memory_order_release);
}

bool Context::top_byte_ignore() const noexcept
{
	return top_byte_ignore_.load(std::memory_order_acquire);
}

GuestEntry Context::guest_entry() const noexcept
{
	return selected_entry_.load(std::memory_order_acquire);
}

std::uint64_t Context::memory_domain() const noexcept
{
	return memory_domain_.load(std::memory_order_acquire);
}

ThreadExecutionStateAccess Context::thread_execution_state()
{
	std::shared_lock lifecycle_lock{execution_lifecycle_mutex_};
	auto &resolved = resolved_thread_execution;
	if (resolved.context == this) {
		if (auto execution = resolved.execution.lock())
			return {std::move(lifecycle_lock), std::move(execution)};
	}
	std::shared_ptr<ThreadStateStore> store;
	{
		std::lock_guard lock{states_mutex_};
		if (!state_store_)
			state_store_ = std::make_shared<ThreadStateStore>();
		store = state_store_;
	}
	auto acquired = store->acquire(std::this_thread::get_id());
	thread_state_registrations.bind(store, acquired.registration);
	resolved.context = this;
	resolved.execution = acquired.execution;
	return {std::move(lifecycle_lock), std::move(acquired.execution)};
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
	: ActiveContextScope{context, DeferredPublication{}}
{
	commit();
}

ActiveContextScope::ActiveContextScope(Context &context,
	DeferredPublication)
	: route_{&context, nullptr,
		::uml::routing::ActiveRouteScope::DeferredPublication{}}
{
}

void ActiveContextScope::commit() noexcept
{
	route_.commit();
}

ActiveContextScope::~ActiveContextScope() = default;

ActiveRunRouteScope::ActiveRunRouteScope(Context &context, void *network,
	DeferredPublication) noexcept
	: route_{&context, network,
		::uml::routing::ActiveRouteScope::DeferredPublication{}}
{
}

ActiveRunRouteScope::~ActiveRunRouteScope() = default;

void ActiveRunRouteScope::commit() noexcept
{
	route_.commit();
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
	const auto route = ::uml::routing::current_route();
	if (route.runtime != nullptr)
		return static_cast<Context *>(route.runtime);
	return route.public_run_active ? nullptr : legacy_context();
}

namespace {

bool provide_memory_domain(std::uint64_t &domain) noexcept
{
	auto *context = current_context();
	if (context == nullptr)
		return false;
	domain = context->memory_domain();
	return domain != 0;
}

} // namespace


bool set_translation_cache_budget(std::size_t total_bytes,
	unsigned executors, std::size_t per_executor_bytes) noexcept
{
	return ::uml::cpu::set_translation_cache_budget(
		total_bytes, executors, per_executor_bytes);
}

bool acquire_translation_cache_run_session() noexcept
{
	return ::uml::cpu::acquire_translation_cache_run_session();
}

void release_translation_cache_run_session() noexcept
{
	::uml::cpu::release_translation_cache_run_session();
}

std::size_t parse_translation_cache_quantity(const char *text) noexcept
{
	return ::uml::cpu::parse_translation_cache_quantity(text);
}

unsigned default_guest_cpu_count() noexcept
{
	return std::clamp(::uml::platform::online_processor_count(), 1U,
		maximum_guest_cpu_count);
}

} // namespace uml::runtime
