// SPDX-License-Identifier: GPL-2.0-or-later
#include "simd_operand_histogram.hpp"

#if UML_GADGET_SIMD_OPERAND_HISTOGRAM

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace uml::cpu::arm64::gadgets::diagnostics {
namespace {

constexpr unsigned register_count = 32U;

using Counts = std::array<std::uint64_t, register_count>;

std::mutex &registry_mutex()
{
	static std::mutex mutex;
	return mutex;
}

std::vector<Counts *> &registry()
{
	static std::vector<Counts *> tables;
	return tables;
}

/* Per-thread counting keeps translation lock-free, and a flat array keeps it
 * allocation-free after the first record on a thread. The tables are registered
 * so the dump can merge them, and deliberately leaked so a thread exiting
 * during teardown cannot race the dump - the same reasoning the adjacent-pair
 * histogram documents. */
Counts &thread_counts()
{
	static thread_local Counts *counts = [] {
		auto *table = new Counts{};
		const std::scoped_lock lock{registry_mutex()};
		registry().push_back(table);
		return table;
	}();
	return *counts;
}

void dump()
{
	const char *path =
		std::getenv("DARWIN_UML_GADGET_SIMD_OPERAND_HISTOGRAM");
	if (path == nullptr)
		return;
	Counts merged{};
	std::uint64_t total = 0;
	{
		const std::scoped_lock lock{registry_mutex()};
		for (const auto *table : registry())
			for (unsigned index = 0; index < register_count; ++index)
				merged[index] += (*table)[index];
	}
	for (const auto count : merged)
		total += count;
	std::FILE *out = std::fopen(path, "w");
	if (out == nullptr)
		return;
	std::fprintf(out, "# total vector operand accesses: %llu\n",
		static_cast<unsigned long long>(total));
	std::fprintf(out, "register\taccesses\n");
	for (unsigned index = 0; index < register_count; ++index)
		std::fprintf(out, "v%u\t%llu\n", index,
			static_cast<unsigned long long>(merged[index]));
	std::fclose(out);
}

const bool dump_registered = [] {
	std::atexit(dump);
	return true;
}();

/* atexit alone is not enough: the harness shuts the emulator down with
 * `poweroff -f` and then SIGTERM, and a run that ends on a signal never runs
 * exit handlers - which is exactly how the adjacent-pair histogram's first
 * capture produced an empty file. Rewrite periodically so the result survives
 * any exit path. Cost is irrelevant in a diagnostic-only build. */
constexpr std::uint64_t dump_interval = 1U << 16;
std::atomic_uint64_t recorded_since_dump{};

} // namespace

void record_simd_operand(unsigned index) noexcept
{
	if (index >= register_count)
		return;
	try {
		++thread_counts()[index];
		if (recorded_since_dump.fetch_add(1, std::memory_order_relaxed) %
			    dump_interval == dump_interval - 1)
			dump();
	} catch (...) {
		/* A diagnostic must never change what the translator does. */
	}
}

} // namespace uml::cpu::arm64::gadgets::diagnostics

#endif
