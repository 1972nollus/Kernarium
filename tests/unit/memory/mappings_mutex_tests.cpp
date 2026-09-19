// SPDX-License-Identifier: GPL-2.0-or-later
#include "mappings_mutex.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace {

using uml::memory::MappingsMutex;

[[nodiscard]] bool expect(bool condition, const char *message) noexcept
{
	if (!condition)
		std::fprintf(stderr, "mappings mutex: %s\n", message);
	return condition;
}

/* Writers exclude each other and every reader: a writer tears the two halves
 * of the pair apart for the length of its hold, and nobody may see that. */
[[nodiscard]] bool writers_exclude_readers_and_each_other()
{
	constexpr unsigned writers = 3;
	constexpr unsigned readers = 5;
	constexpr unsigned rounds = 20000;
	MappingsMutex mutex;
	std::uint64_t first = 0;
	std::uint64_t second = 0;
	std::atomic<bool> torn{false};
	std::atomic<bool> done{false};
	std::vector<std::thread> threads;
	for (unsigned index = 0; index < readers; ++index) {
		threads.emplace_back([&] {
			while (!done.load(std::memory_order_relaxed)) {
				std::shared_lock lock{mutex};
				if (first != second)
					torn.store(true, std::memory_order_relaxed);
			}
		});
	}
	std::vector<std::thread> writing;
	for (unsigned index = 0; index < writers; ++index) {
		writing.emplace_back([&] {
			for (unsigned round = 0; round < rounds; ++round) {
				std::lock_guard lock{mutex};
				++first;
				if (first == second)
					torn.store(true, std::memory_order_relaxed);
				++second;
			}
		});
	}
	for (auto &thread : writing)
		thread.join();
	done.store(true, std::memory_order_relaxed);
	for (auto &thread : threads)
		thread.join();
	return expect(!torn.load(), "a holder saw another writer's update") &&
		expect(first == writers * rounds && second == first,
			"writers lost updates");
}

/* Readers share: a second reader gets in while the first still holds. */
[[nodiscard]] bool readers_hold_the_lock_together()
{
	MappingsMutex mutex;
	std::shared_lock first{mutex};
	std::atomic<bool> entered{false};
	std::thread second{[&] {
		std::shared_lock lock{mutex};
		entered.store(true);
	}};
	second.join();
	const bool writer_kept_out = !mutex.try_lock();
	first.unlock();
	const bool writer_admitted = mutex.try_lock();
	if (writer_admitted)
		mutex.unlock();
	return expect(entered.load(), "a second reader waited for the first") &&
		expect(writer_kept_out, "a writer entered beside a reader") &&
		expect(writer_admitted, "a writer was refused a free lock");
}

/* A steady stream of overlapping readers does not starve a writer: a waiting
 * writer holds new readers off. */
[[nodiscard]] bool a_waiting_writer_is_not_starved()
{
	constexpr unsigned readers = 4;
	MappingsMutex mutex;
	std::atomic<bool> done{false};
	std::vector<std::thread> threads;
	for (unsigned index = 0; index < readers; ++index) {
		threads.emplace_back([&] {
			while (!done.load(std::memory_order_relaxed)) {
				std::shared_lock lock{mutex};
				for (volatile unsigned spin = 0; spin < 200; spin = spin + 1) {
				}
			}
		});
	}
	std::this_thread::sleep_for(std::chrono::milliseconds{20});
	const auto started = std::chrono::steady_clock::now();
	unsigned acquired = 0;
	while (acquired < 1000 &&
	       std::chrono::steady_clock::now() - started <
		       std::chrono::seconds{10}) {
		std::lock_guard lock{mutex};
		++acquired;
	}
	done.store(true, std::memory_order_relaxed);
	for (auto &thread : threads)
		thread.join();
	return expect(acquired == 1000,
		"a writer starved behind overlapping readers");
}

} // namespace

int main()
{
	const bool passed = writers_exclude_readers_and_each_other() &&
		readers_hold_the_lock_together() &&
		a_waiting_writer_is_not_starved();
	return passed ? 0 : 1;
}
