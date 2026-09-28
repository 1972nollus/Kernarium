// SPDX-License-Identifier: GPL-2.0-or-later
/* The native window's bounds are read as a sequence-stamped copy, without a
 * lock. Both loads must stay ahead of the closing revision check, or a reader
 * takes the start of one window and the end of another and still accepts the
 * snapshot. Readers here classify an address that lies outside both windows a
 * writer alternates between, and must never find it inside one. Without the
 * reader's fence they do so tens of thousands of times a second.
 */
#include "address_layout.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

int main()
{
	using namespace std::chrono_literals;
	using uml::memory::AddressDomain;
	using uml::memory::ConfiguredAddressLayout;
	constexpr std::uint64_t gib = 1ULL << 30U;
	constexpr std::uint64_t base = DARWIN_UML_NATIVE_ADDRESS_START;
	static_assert(base + 6U * gib <= DARWIN_UML_GUEST_ADDRESS_LIMIT);
	/* Inside only the mixed window [first start, second end). */
	constexpr std::uint64_t between = base + 3U * gib;
	std::atomic<bool> stop{};
	std::atomic<bool> refused{};
	std::atomic<unsigned long> reads{};
	std::atomic<unsigned long> torn{};
	/* The default window covers `between`, so the readers start after the
	 * first of the two is in place. */
	if (!uml::memory::configure_native_address_window(base, base + 2U * gib)) {
		std::fprintf(stderr, "a native window was refused\n");
		return EXIT_FAILURE;
	}
	std::vector<std::thread> readers;
	for (int index = 0; index < 4; ++index)
		readers.emplace_back([&] {
			unsigned long local_reads = 0;
			unsigned long local_torn = 0;
			while (!stop.load(std::memory_order_relaxed)) {
				++local_reads;
				if (ConfiguredAddressLayout::classify(between, 1) !=
				    AddressDomain::guest)
					++local_torn;
			}
			reads.fetch_add(local_reads, std::memory_order_relaxed);
			torn.fetch_add(local_torn, std::memory_order_relaxed);
		});
	std::thread writer{[&] {
		while (!stop.load(std::memory_order_relaxed)) {
			if (!uml::memory::configure_native_address_window(
				    base, base + 2U * gib) ||
			    !uml::memory::configure_native_address_window(
				    base + 4U * gib, base + 6U * gib))
				refused.store(true, std::memory_order_relaxed);
		}
	}};
	std::this_thread::sleep_for(400ms);
	stop.store(true, std::memory_order_relaxed);
	writer.join();
	for (auto &reader : readers)
		reader.join();
	uml::memory::reset_native_address_window();
	if (refused.load()) {
		std::fprintf(stderr, "a native window was refused\n");
		return EXIT_FAILURE;
	}
	if (torn.load() != 0) {
		std::fprintf(stderr, "%lu of %lu window reads were torn\n",
			torn.load(), reads.load());
		return EXIT_FAILURE;
	}
	std::printf("PASS: %lu window reads, none torn\n", reads.load());
	return EXIT_SUCCESS;
}
