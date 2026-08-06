// SPDX-License-Identifier: GPL-2.0-or-later
#include "run_routing.hpp"

#if UML_RUN_ROUTING_TESTING
#include "run_routing_testing.hpp"

#include <chrono>
#include <condition_variable>
#endif

#include <cstddef>
#include <mutex>

namespace uml::routing {
namespace {

struct RouteRegistry final {
	std::mutex mutex;
	std::size_t active_count{};
	ActiveRouteScope *sole_fallback{};
};

/* Public routes can outlive ordinary static destruction during process exit.
 * Give the synchronization object explicit process lifetime. */
RouteRegistry *const route_registry = new RouteRegistry;
thread_local ActiveRouteScope *thread_route;

#if UML_RUN_ROUTING_TESTING
struct TestTransactionBarrier final {
	std::mutex mutex;
	std::condition_variable changed;
	testing::TransactionBoundary boundary{
		testing::TransactionBoundary::commit};
	bool armed{};
	bool paused{};
	bool resolver_waiting{};
};

TestTransactionBarrier *const test_barrier = new TestTransactionBarrier;

void pause_transaction(testing::TransactionBoundary boundary) noexcept
{
	std::unique_lock lock{test_barrier->mutex};
	if (!test_barrier->armed || test_barrier->boundary != boundary)
		return;
	test_barrier->paused = true;
	test_barrier->changed.notify_all();
	test_barrier->changed.wait(lock, [] {
		return test_barrier->resolver_waiting || !test_barrier->armed;
	});
	test_barrier->armed = false;
	test_barrier->paused = false;
	test_barrier->resolver_waiting = false;
}

void announce_resolver() noexcept
{
	std::lock_guard lock{test_barrier->mutex};
	if (!test_barrier->paused)
		return;
	test_barrier->resolver_waiting = true;
	test_barrier->changed.notify_all();
}
#endif

} // namespace

ActiveRouteScope::ActiveRouteScope(void *runtime, void *network) noexcept
	: ActiveRouteScope{runtime, network, DeferredPublication{}}
{
	commit();
}

ActiveRouteScope::ActiveRouteScope(void *runtime, void *network,
	DeferredPublication) noexcept
	: route_{runtime, network, true}
{
}

void ActiveRouteScope::commit() noexcept
{
	if (committed_)
		return;
	std::lock_guard lock{route_registry->mutex};
	previous_thread_ = thread_route;
	route_registry->sole_fallback =
		route_registry->active_count == 0 ? this : nullptr;
	++route_registry->active_count;
	committed_ = true;
	thread_route = this;
#if UML_RUN_ROUTING_TESTING
	pause_transaction(testing::TransactionBoundary::commit);
#endif
}

ActiveRouteScope::~ActiveRouteScope()
{
	if (committed_) {
		std::lock_guard lock{route_registry->mutex};
		if (route_registry->sole_fallback == this)
			route_registry->sole_fallback = nullptr;
		--route_registry->active_count;
#if UML_RUN_ROUTING_TESTING
		pause_transaction(testing::TransactionBoundary::retirement);
#endif
	}
	if (committed_)
		thread_route = previous_thread_;
}

Route current_route() noexcept
{
	if (thread_route != nullptr)
		return thread_route->route_;
#if UML_RUN_ROUTING_TESTING
	announce_resolver();
#endif
	std::lock_guard lock{route_registry->mutex};
	if (route_registry->sole_fallback != nullptr)
		return route_registry->sole_fallback->route_;
	return {.public_run_active = route_registry->active_count != 0};
}

#if UML_RUN_ROUTING_TESTING
namespace testing {

void pause_next_transaction(TransactionBoundary boundary) noexcept
{
	std::lock_guard lock{test_barrier->mutex};
	test_barrier->boundary = boundary;
	test_barrier->armed = true;
	test_barrier->paused = false;
	test_barrier->resolver_waiting = false;
}

bool wait_for_transaction_pause(unsigned int timeout_milliseconds) noexcept
{
	std::unique_lock lock{test_barrier->mutex};
	return test_barrier->changed.wait_for(lock,
		std::chrono::milliseconds{timeout_milliseconds}, [] {
			return test_barrier->paused;
		});
}

void cancel_transaction_pause() noexcept
{
	std::lock_guard lock{test_barrier->mutex};
	test_barrier->armed = false;
	test_barrier->changed.notify_all();
}

} // namespace testing
#endif

} // namespace uml::routing
