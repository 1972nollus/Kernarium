// SPDX-License-Identifier: GPL-2.0-or-later
#include "dependency_coverage.hpp"

#if UML_GADGET_DEPENDENCY_COVERAGE

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace uml::cpu::arm64::gadgets::diagnostics {
namespace {

using Counters = std::array<std::atomic_uint64_t, dependency_counter_count>;
static_assert(sizeof(std::atomic_uint64_t) == sizeof(std::uint64_t));
static_assert(std::atomic_uint64_t::is_always_lock_free);

/* Each carrier has exactly one writer. Aligned A64 LDR/STR implements the
 * relaxed atomic operations used by the threaded counter gadget. The sampler
 * uses relaxed loads; snapshots across carriers are intentionally not one
 * globally atomic instant. Tables outlive carriers and remain valid at exit.
 */
class Coverage final {
public:
	Coverage()
	{
		const auto *path = std::getenv("DARWIN_UML_GADGET_DEPENDENCY_COVERAGE");
		if (path == nullptr || *path == '\0')
			return;
		output_ = std::fopen(path, "w");
		if (output_ == nullptr) {
			std::perror("gadget dependency coverage");
			std::abort();
		}
		std::fprintf(output_, "monotonic_ns,entered_guest_words,unsupported_groups");
		for (const auto *family : scalar_family_names)
			for (unsigned sources = 0; sources < 4; ++sources)
				for (unsigned mask = 0; mask < 8; ++mask)
					std::fprintf(output_, ",%s_s%u_m%u", family, sources, mask);
		for (unsigned sources = 0; sources < 4; ++sources)
			for (unsigned mask = 0; mask < 8; ++mask)
				std::fprintf(output_, ",simd_s%u_m%u", sources, mask);
		std::fputc('\n', output_);
		std::fflush(output_);
		worker_ = std::thread{[this] {
			while (!stop_.load(std::memory_order_relaxed)) {
				dump();
				std::this_thread::sleep_for(std::chrono::milliseconds{100});
			}
		}};
	}
	~Coverage()
	{
		stop_.store(true, std::memory_order_relaxed);
		if (worker_.joinable())
			worker_.join();
		if (output_ != nullptr) {
			dump();
			std::fclose(output_);
		}
	}
	Counters *register_thread()
	{
		auto *result = new Counters{};
		const std::scoped_lock lock{mutex_};
		tables_.push_back(result);
		return result;
	}
	DependencyCounts snapshot()
	{
		DependencyCounts result{};
		const std::scoped_lock lock{mutex_};
		for (const auto *table : tables_)
			for (std::size_t i = 0; i < result.size(); ++i)
				result[i] += (*table)[i].load(std::memory_order_relaxed);
		return result;
	}

private:
	void dump()
	{
		const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		const auto values = snapshot();
		std::fprintf(output_, "%lld", static_cast<long long>(now));
		for (const auto value : values)
			std::fprintf(output_, ",%llu", static_cast<unsigned long long>(value));
		std::fputc('\n', output_);
		std::fflush(output_);
	}
	std::mutex mutex_;
	std::vector<Counters *> tables_;
	std::atomic_bool stop_{};
	std::thread worker_;
	std::FILE *output_{};
};

Coverage &coverage()
{
	static Coverage instance;
	return instance;
}

} // namespace

void *dependency_counters()
{
	static thread_local auto *counters = coverage().register_thread();
	return counters->data();
}

DependencyCounts dependency_snapshot()
{
	return coverage().snapshot();
}

} // namespace uml::cpu::arm64::gadgets::diagnostics
#endif
