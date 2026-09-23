// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/gadgets/execution_histogram.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/gadgets/tlb.hpp"
#include "memory/static_guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace uml::cpu::arm64::gadgets {

struct SharedKey;

struct GadgetCacheDiagnostics final {
	/* Programs this executor had to produce, whether it built them or took
	 * them from another executor. */
	std::uint64_t translations{};
	/* Of those, the ones another executor had already built and this one
	 * only had to copy and re-point at its own link records. */
	std::uint64_t adoptions{};
	std::uint64_t validation_attempts{};
	std::uint64_t validation_hits{};
	std::uint64_t validation_mismatches{};
	std::uint64_t validation_faults{};
	std::uint64_t validation_sequence_races{};
#if UML_GADGET_EXECUTION_HISTOGRAM
	/* Why translated execution returned to this executor, per block entry:
	 * the chain reached its linked-instruction limit, an indirect site missed
	 * both of its cells, an unlinked or exit gadget left with the PC set, or
	 * a guest-visible event (syscall, fault, interrupt, other) ended it. */
	std::uint64_t block_entries{};
	std::uint64_t exits_chain_limit{};
	std::uint64_t exits_indirect_miss{};
	std::uint64_t exits_indirect_miss_return{};
	/* Misses at sites with 1, 2, 3, 4, 5-8, 9-16, 17-64, >64 distinct
	 * targets, refreshed by the executor on each miss. */
	std::array<std::uint64_t, 8> miss_by_site_targets{};
	std::uint64_t exits_unlinked{};
	std::uint64_t exits_syscall{};
	std::uint64_t exits_fault{};
	std::uint64_t exits_interrupt{};
	std::uint64_t exits_other{};
	std::uint64_t cache_misses{};
#endif
};

class GadgetExecutor final {
public:
	explicit GadgetExecutor(
		BuildOptions build_options = {});
	~GadgetExecutor();
	[[nodiscard]] bool begin_execution(unsigned long generation,
					   std::uint64_t guest_pc);
	void end_execution() noexcept;

	[[nodiscard]] ::uml::cpu::EngineEvent run_block(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::size_t &executed);
	[[nodiscard]] bool preempted() const noexcept { return preempted_; }
	/* While set, every run_block retires at most one guest instruction. */
	void set_single_step(bool enabled) noexcept { single_step_ = enabled; }

	[[nodiscard]] std::uint64_t fault_address() const noexcept;
	[[nodiscard]] ::uml::memory::Access fault_access() const noexcept;
	[[nodiscard]] bool fault_alignment() const noexcept;
	[[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept;
	[[nodiscard]] const GadgetCacheDiagnostics &cache_diagnostics() const noexcept
	{
		return cache_diagnostics_;
	}

private:
	/* A cached translation is handed out as a BuildResult whose program
	 * borrows the cache's words, so it is valid until the next store. */
	void prepare_cache(unsigned long generation, std::uint64_t changes);
	void select_cpu(CpuState &cpu) noexcept;
	[[nodiscard]] std::optional<BuildResult> find(
		std::uint64_t pc, std::uint64_t changes,
		::uml::memory::StaticGuestMemory &memory);
	[[nodiscard]] BuildResult translate(
		std::uint64_t pc, std::span<const std::uint32_t> window);
	[[nodiscard]] BuildResult build(
		std::uint64_t pc, std::span<const std::uint32_t> window,
		const SharedKey *key);
	[[nodiscard]] BuildResult store(
		std::uint64_t pc, const BuildResult &result,
		std::span<const std::uint32_t> guest_words);
	[[nodiscard]] void *execution_context(unsigned long generation);
	[[nodiscard]] ::uml::cpu::EngineEvent run_trap_enabled_instruction(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		std::size_t &executed);

	class ProgramCache;

	unsigned long cache_generation_{};
	std::uint64_t cache_changes_{};
	unsigned long mapping_generation_{};
	std::uint64_t mapping_changes_{};
	std::unique_ptr<ProgramCache> programs_;
	::uml::memory::Tlb tlb_;
	CpuState *current_cpu_{};
	void *execution_context_{};
	unsigned long execution_generation_{};
	bool preempted_{};
	bool single_step_{};
	std::uint32_t floating_point_syndrome_{};
	BuildOptions build_options_;
	/* Which translations this executor's builds are interchangeable with;
	 * derived once from build_options_, since gadget selection is fixed for
	 * the executor's lifetime. */
	std::uint64_t profile_{};
	/* The memory a build may read a callee's instructions through, set for
	 * the duration of one run_block. */
	::uml::memory::StaticGuestMemory *build_memory_{};
	GadgetCacheDiagnostics cache_diagnostics_{};
};

} // namespace uml::cpu::arm64::gadgets
