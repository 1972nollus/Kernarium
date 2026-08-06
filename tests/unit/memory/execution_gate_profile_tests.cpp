// SPDX-License-Identifier: GPL-2.0-or-later
#include "execution_gate.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <thread>
#include <vector>

namespace {

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

} // namespace

int main()
{
	require(::setenv("UML_PROFILE_TLB_GATE", "1", 1) == 0);
	require(uml::memory::testing::execution_gate_profile_count() == 0);

	uml::memory::GenerationState state;
	for (unsigned round = 0; round < 16; ++round) {
		std::atomic_bool failed{};
		std::vector<std::thread> threads;
		threads.reserve(64);
		for (unsigned index = 0; index < 64; ++index) {
			threads.emplace_back([&, index] {
				if (!uml::memory::execution_reader_begin(
						state, round + 1, 0x1000 + index * 4)) {
					failed.store(true, std::memory_order_release);
					return;
				}
				uml::memory::execution_reader_end(state);
			});
		}
		for (auto &thread : threads)
			thread.join();
		require(!failed.load(std::memory_order_acquire));
		require(uml::memory::testing::execution_gate_profile_count() == 0);
	}

	std::puts("execution-gate profile registration churn tests passed");
	return EXIT_SUCCESS;
}
