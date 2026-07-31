// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::backend::arm64::detail {

/* AArch64 maps each FPCR exception-enable bit eight positions above the
 * matching cumulative FPSR bit.  Never install these guest enables in the
 * host FPCR: the emulator reports the guest trap after a transactional
 * instruction instead of allowing a host floating-point exception.
 */
inline constexpr std::uint32_t fpcr_exception_enable_mask = 0x0000'9f00U;
inline constexpr std::uint32_t fpsr_exception_cumulative_mask = 0x0000'009fU;

[[nodiscard]] constexpr std::uint32_t host_fpcr(
	std::uint32_t guest_fpcr) noexcept
{
	return guest_fpcr & ~fpcr_exception_enable_mask;
}

[[nodiscard]] constexpr std::uint32_t enabled_exception_flags(
	std::uint32_t guest_fpcr) noexcept
{
	return (guest_fpcr >> 8U) & fpsr_exception_cumulative_mask;
}

/* The 0b111 major opcode class is the register-only FP/Advanced-SIMD
 * data-processing space.  In particular this excludes every SIMD/FP memory
 * instruction, so an enabled guest FP trap never makes a memory operation
 * transactional.
 */
[[nodiscard]] constexpr bool is_fp_simd_data_processing(
	std::uint32_t instruction) noexcept
{
	return (instruction & 0x0e00'0000U) == 0x0e00'0000U;
}

[[nodiscard]] constexpr bool needs_floating_exception_transaction(
	std::uint32_t guest_fpcr, std::uint32_t instruction) noexcept
{
	return (guest_fpcr & fpcr_exception_enable_mask) != 0U &&
		is_fp_simd_data_processing(instruction);
}

/* ESR_ELx: EC=0x2c (FP exception from AArch64), IL=1, TFV=1, followed by
 * the architecturally ordered FPSR cause flags in the low ISS bits.
 */
[[nodiscard]] constexpr std::uint32_t floating_exception_syndrome(
	std::uint32_t raised_enabled) noexcept
{
	return (0x2cU << 26U) | (1U << 25U) | (1U << 23U) |
		(raised_enabled & fpsr_exception_cumulative_mask);
}

struct FloatingExceptionDecision final {
	std::uint32_t newly_raised{};
	std::uint32_t raised_enabled{};
	std::uint32_t committed_fpsr{};
	std::uint32_t syndrome{};

	[[nodiscard]] constexpr bool traps() const noexcept
	{
		return raised_enabled != 0U;
	}
};

/* isolated_fpsr is the result of executing with all cumulative exception
 * flags initially clear.  A trap commits only newly raised, unenabled flags;
 * all other architectural state remains at the pre-instruction value.
 */
[[nodiscard]] constexpr FloatingExceptionDecision
decide_floating_exception(std::uint32_t guest_fpcr,
	std::uint32_t original_fpsr, std::uint32_t isolated_fpsr) noexcept
{
	const auto newly_raised =
		isolated_fpsr & fpsr_exception_cumulative_mask;
	const auto raised_enabled =
		newly_raised & enabled_exception_flags(guest_fpcr);
	const auto committed_fpsr = original_fpsr |
		(newly_raised & ~enabled_exception_flags(guest_fpcr));
	return {
		newly_raised,
		raised_enabled,
		committed_fpsr,
		floating_exception_syndrome(raised_enabled),
	};
}

} // namespace uml::backend::arm64::detail
