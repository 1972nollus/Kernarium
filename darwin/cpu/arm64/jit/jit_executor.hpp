// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/engine.hpp"
#include "memory/tlb.hpp"
#include "memory/static_guest_memory.hpp"

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
#include <array>
#endif
#include <cstddef>
#include <cstdint>
#include <memory>

namespace uml::cpu::arm64 {

struct JitExecutorOptions final {
	bool enable_guarded_control_flow{};
	bool enable_pointer_authentication{true};
	bool reuse_validated_stale_blocks{true};
	/* Zero selects the production arena size. */
	std::size_t code_arena_size{};
	::uml::cpu::HostCapabilities host_capabilities{
		::uml::cpu::host_capabilities()};
};

struct JitCacheValidationDiagnostics final {
	std::uint64_t translations{};
	std::uint64_t validation_attempts{};
	std::uint64_t validation_hits{};
	std::uint64_t validation_mismatches{};
	std::uint64_t validation_faults{};
	std::uint64_t validation_sequence_races{};
	std::uint64_t cache_replacements{};
	std::uint64_t arena_resets{};
};

struct JitIndirectLinkDiagnostics final {
	std::uint64_t misses{};
	std::uint64_t trains{};
	std::uint64_t unsafe_target_refusals{};
	std::uint64_t scope_rejections{};
	std::uint64_t deactivated_sites{};
	std::uint64_t capacity_refusals{};
};

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
struct JitGprLinkProfileDiagnostics final {
	std::uint64_t successful_links{};
	std::uint64_t unattributed_links{};
	std::uint64_t outer_invocations{};
	std::array<std::uint64_t, 32> live_in_saves{};
	std::array<std::uint64_t, 32> dirty_out_saves{};
	std::array<std::uint64_t, 32> full_handoffs{};
};

[[nodiscard]] JitGprLinkProfileDiagnostics
global_gpr_link_profile_diagnostics() noexcept;
void report_global_gpr_link_profile() noexcept;
#endif

class JitExecutor final {
public:
	explicit JitExecutor(bool enable_guarded_control_flow = false);
	explicit JitExecutor(JitExecutorOptions options);
	~JitExecutor();

	JitExecutor(const JitExecutor &) = delete;
	JitExecutor &operator=(const JitExecutor &) = delete;

	[[nodiscard]] bool begin_execution(unsigned long generation,
					   std::uint64_t guest_pc);
	void end_execution() noexcept;
	[[nodiscard]] ::uml::cpu::EngineEvent run_block(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::size_t &executed);
	[[nodiscard]] bool preempted() const noexcept;
	[[nodiscard]] std::uint64_t fault_address() const noexcept;
	[[nodiscard]] ::uml::memory::Access fault_access() const noexcept;
	[[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept;
	[[nodiscard]] JitCacheValidationDiagnostics
	cache_validation_diagnostics() const noexcept;
	[[nodiscard]] JitIndirectLinkDiagnostics
	indirect_link_diagnostics() const noexcept;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
	[[nodiscard]] JitGprLinkProfileDiagnostics
	gpr_link_profile_diagnostics() noexcept;
#endif

private:
	class Implementation;
	std::unique_ptr<Implementation> implementation_;
};

[[nodiscard]] bool native_jit_available() noexcept;

} // namespace uml::cpu::arm64
