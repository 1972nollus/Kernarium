// SPDX-License-Identifier: GPL-2.0-or-later
/* An unbound thread reads the published route as a sequence-stamped copy,
 * without a lock. Its payload loads must stay ahead of the closing sequence
 * check, or a reader pairs one publication's runtime with another's network
 * and still accepts the snapshot. Readers here race a writer that alternates
 * two runs for a bounded time, against the production object, and must never
 * see a mixed route. Without the reader's fence they see thousands a second.
 */
#include "run_routing.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

int main()
{
	using namespace std::chrono_literals;
	static int runtime_a;
	static int network_a;
	static int runtime_b;
	static int network_b;
	std::atomic<bool> stop{};
	std::atomic<unsigned long> reads{};
	std::atomic<unsigned long> torn{};
	std::vector<std::thread> readers;
	for (int index = 0; index < 4; ++index)
		readers.emplace_back([&] {
			unsigned long local_reads = 0;
			unsigned long local_torn = 0;
			while (!stop.load(std::memory_order_relaxed)) {
				const auto route = uml::routing::current_route();
				++local_reads;
				const bool coherent =
					(route.runtime == &runtime_a &&
					 route.network == &network_a &&
					 route.public_run_active) ||
					(route.runtime == &runtime_b &&
					 route.network == &network_b &&
					 route.public_run_active) ||
					(route.runtime == nullptr &&
					 route.network == nullptr);
				if (!coherent)
					++local_torn;
			}
			reads.fetch_add(local_reads, std::memory_order_relaxed);
			torn.fetch_add(local_torn, std::memory_order_relaxed);
		});
	std::thread writer{[&] {
		while (!stop.load(std::memory_order_relaxed)) {
			{
				uml::routing::ActiveRouteScope scope{
					&runtime_a, &network_a};
			}
			{
				uml::routing::ActiveRouteScope scope{
					&runtime_b, &network_b};
			}
		}
	}};
	std::this_thread::sleep_for(400ms);
	stop.store(true, std::memory_order_relaxed);
	writer.join();
	for (auto &reader : readers)
		reader.join();
	if (torn.load() != 0) {
		std::fprintf(stderr, "%lu of %lu unbound route reads were torn\n",
			torn.load(), reads.load());
		return EXIT_FAILURE;
	}
	std::printf("PASS: %lu unbound route reads, none torn\n", reads.load());
	return EXIT_SUCCESS;
}
