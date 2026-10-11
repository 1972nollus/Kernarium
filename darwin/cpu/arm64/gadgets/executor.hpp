// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/detail/sme_encodings.hpp"
#include "cpu/arm64/gadgets/execution_histogram.hpp"
#include "cpu/arm64/gadgets/guest_sampler.hpp"
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

struct GadgetCacheDiagnostics final {
	/* Programs this executor had to produce. */
	std::uint64_t translations{};
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

class GadgetExecutor final : diagnostics::SampledExecutor {
public:
	explicit GadgetExecutor(
		BuildOptions build_options = {});
	~GadgetExecutor();
	GadgetExecutor(const GadgetExecutor &) = delete;
	GadgetExecutor &operator=(const GadgetExecutor &) = delete;
	[[nodiscard]] bool begin_execution(unsigned long generation,
					   std::uint64_t guest_pc);
	void end_execution() noexcept;

	[[nodiscard]] ::uml::cpu::EngineEvent run_block(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::size_t &executed);
	/* run_block for an AArch32 task (a32/a32_builder.hpp): cpu.pc is an
	 * A32 or T32 key (aarch32/guest_key.hpp) and every exit leaves it one.
	 * Retirement, fault positions and ITSTATE follow the contract in
	 * aarch32/README.md; a breakpoint exit leaves BKPT's immediate in
	 * breakpoint_immediate(). */
	[[nodiscard]] ::uml::cpu::EngineEvent run_a32_block(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::size_t &executed);
	[[nodiscard]] std::uint32_t breakpoint_immediate() const noexcept
	{
		return breakpoint_immediate_;
	}
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
	[[nodiscard]] bool sample_block(const void *stream,
		diagnostics::SampledBlock &block) const noexcept override;

	/* A cached translation is handed out as a BuildResult whose program
	 * borrows the cache's words, so it is valid until the next store. */
	void prepare_cache(unsigned long generation, std::uint64_t changes);
	void select_cpu(CpuState &cpu) noexcept;
	[[nodiscard]] std::optional<BuildResult> find(
		std::uint64_t pc, std::uint64_t changes,
		::uml::memory::StaticGuestMemory &memory);
	[[nodiscard]] BuildResult translate(
		std::uint64_t pc, std::span<const std::uint32_t> window);
	[[nodiscard]] void *execution_context(unsigned long generation);
	[[nodiscard]] ::uml::cpu::EngineEvent run_trap_enabled_instruction(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		std::size_t &executed);
	/* One SVE word outside streaming mode, implemented as `row`, through
	 * the semantic library, for the trap-enabled path. */
	[[nodiscard]] ::uml::cpu::EngineEvent run_library_instruction(
		CpuState &cpu, std::uint32_t instruction,
		const detail::SmeEncoding &row, std::size_t &executed);

	class ProgramCache;

	/* run_block for a guest SVCR other than 0, out of line so that the
	 * mode-0 path stays as it was. */
	[[nodiscard]] ::uml::cpu::EngineEvent run_sme_block(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::uint64_t code_changes,
		std::size_t &executed);
	/* What both paths do once a program has run: correct a terminal
	 * exit's retirement count and train the indirect site it missed. */
	[[nodiscard]] ::uml::cpu::EngineEvent finish_block(
		ProgramCache &programs, const BuildResult &built,
		::uml::cpu::EngineEvent event, CpuState &cpu,
		std::size_t &executed);

	/* What both AArch32 paths do once a program has run: the A64 correction
	 * of a terminal exit's retirement, made in guest units a T32 block's
	 * instructions do not fill, ITSTATE for a T32 exit, and the indirect
	 * site's training. */
	[[nodiscard]] ::uml::cpu::EngineEvent finish_a32_block(
		ProgramCache &programs, ::uml::cpu::EngineEvent event,
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		std::size_t &executed);
	[[nodiscard]] ::uml::cpu::EngineEvent run_a32_instruction(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		std::size_t &executed);
	[[nodiscard]] ::uml::cpu::EngineEvent run_a32_translation(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::uint64_t code_changes,
		std::size_t &executed);

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
	GadgetCacheDiagnostics cache_diagnostics_{};
	/* Null unless DARWIN_UML_GUEST_SAMPLES is set. */
	diagnostics::GuestSamplingSlot *sampling_{};
	/* The translations of SVCR modes 1-3 (SM, ZA, both), each mode in a
	 * cache of its own made on first use; programs_ holds mode 0. A
	 * translation's legality depends on its mode, and keeping each mode's
	 * links, indirect sites and target table apart means no program can
	 * reach another mode's code. */
	struct SmeModeCache final {
		std::unique_ptr<ProgramCache> programs;
		unsigned long generation{};
		std::uint64_t changes{};
	};
	std::array<SmeModeCache, 3> sme_caches_{};
	/* BKPT's immediate, after the last A64 member so that none moves. */
	std::uint32_t breakpoint_immediate_{};
};

} // namespace uml::cpu::arm64::gadgets
