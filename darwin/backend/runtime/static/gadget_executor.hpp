// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/gadgets/tlb.hpp"
#include "runtime/static/guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace uml::backend {

class GadgetExecutor final {
public:
	GadgetExecutor();
	~GadgetExecutor();
	[[nodiscard]] bool begin_execution(unsigned long generation,
					   std::uint64_t guest_pc);
	void end_execution() noexcept;

	[[nodiscard]] arm64::gadgets::Event run_block(
		arm64::CpuState &cpu, StaticGuestMemory &memory,
		unsigned long generation, std::size_t &executed);
	[[nodiscard]] bool preempted() const noexcept { return preempted_; }

	[[nodiscard]] std::uint64_t fault_address() const noexcept;
	[[nodiscard]] arm64::gadgets::Access fault_access() const noexcept;

private:
	using BuildResult = arm64::gadgets::BuildResult;
	using CachedResult = std::reference_wrapper<const BuildResult>;

	void prepare_cache(unsigned long generation, std::uint64_t changes);
	[[nodiscard]] std::optional<CachedResult> find(std::uint64_t pc) const;
	[[nodiscard]] const BuildResult &store(
		std::uint64_t pc, BuildResult result);
	[[nodiscard]] void *execution_context(unsigned long generation);

	class ProgramCache;

	unsigned long cache_generation_{};
	std::uint64_t cache_changes_{};
	unsigned long mapping_generation_{};
	std::uint64_t mapping_changes_{};
	std::unique_ptr<ProgramCache> programs_;
	arm64::gadgets::Tlb tlb_;
	void *execution_context_{};
	unsigned long execution_generation_{};
	bool preempted_{};
};

} // namespace uml::backend
