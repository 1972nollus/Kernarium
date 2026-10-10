// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/semantics_vector.hpp"
#include "cpu/arm64/detail/floating_exception.hpp"

#include <cstdint>

/*
 * The host's own floating-point instructions as the library's VFP and
 * Advanced SIMD arithmetic: each operation is one A64 scalar instruction on
 * bit patterns, run with the FPCR the AArch32 instruction defines (the
 * guest FPSCR's control bits for VFP, the Standard FPSCR value for Advanced
 * SIMD) and the cumulative flags it raises merged into the guest FPSR, as
 * the SVE library does. Overloads on std::uint32_t and std::uint64_t select
 * the single and double precision instruction.
 */

namespace uml::cpu::arm64::aarch32::detail::fp {

inline constexpr std::uint32_t fpcr_fz = 1U << 24;
inline constexpr std::uint32_t fpcr_dn = 1U << 25;
inline constexpr std::uint32_t fpcr_ahp = 1U << 26;
inline constexpr std::uint32_t fpcr_rounding = 3U << 22;

/* The FPCR an Advanced SIMD floating-point operation runs under: the
 * Standard FPSCR value, flush-to-zero and default NaN with round to nearest,
 * keeping the half-precision format and flush controls. */
[[nodiscard]] constexpr std::uint32_t standard_fpcr(std::uint32_t guest) noexcept
{
	return fpcr_fz | fpcr_dn | (guest & (fpcr_ahp | (1U << 19)));
}

/* Runs `function` with `local` installed as the host FPCR and FPSR clear,
 * restores both, and merges the flags it raised into the guest FPSR. The
 * function issues its floating-point instructions as asm volatile, as the
 * operations below do. */
template<typename Function>
inline auto run(CpuState &cpu, std::uint32_t local, Function &&function) noexcept
{
	const auto result = ::uml::cpu::arm64::detail::run_with_local_fpcr(
		std::uint64_t{local}, static_cast<Function &&>(function));
	cpu.fpsr |= result.fpsr;
	return result.value;
}

/* Run under the guest FPSCR (VFP) or the Standard FPSCR value (Advanced
 * SIMD). */
template<typename Function>
inline auto run_effective(CpuState &cpu, Function &&function) noexcept
{
	return run(cpu, cpu.fpcr & fpscr_control_mask, static_cast<Function &&>(function));
}
template<typename Function>
inline auto run_standard(CpuState &cpu, Function &&function) noexcept
{
	return run(cpu, standard_fpcr(cpu.fpcr), static_cast<Function &&>(function));
}

#define UML_A32_FP_BINARY(name, insn)                                                  \
	[[nodiscard]] inline std::uint32_t name(std::uint32_t a, std::uint32_t b) noexcept \
	{                                                                                  \
		std::uint32_t r;                                                               \
		__asm__ volatile("fmov s1, %w1\n\tfmov s2, %w2\n\t" insn " s0, s1, s2\n\t"     \
				 "fmov %w0, s0"                                                        \
				 : "=r"(r)                                                             \
				 : "r"(a), "r"(b)                                                      \
				 : "v0", "v1", "v2");                                                  \
		return r;                                                                      \
	}                                                                                  \
	[[nodiscard]] inline std::uint64_t name(std::uint64_t a, std::uint64_t b) noexcept \
	{                                                                                  \
		std::uint64_t r;                                                               \
		__asm__ volatile("fmov d1, %1\n\tfmov d2, %2\n\t" insn " d0, d1, d2\n\t"       \
				 "fmov %0, d0"                                                         \
				 : "=r"(r)                                                             \
				 : "r"(a), "r"(b)                                                      \
				 : "v0", "v1", "v2");                                                  \
		return r;                                                                      \
	}

#define UML_A32_FP_UNARY(name, insn)                                       \
	[[nodiscard]] inline std::uint32_t name(std::uint32_t a) noexcept      \
	{                                                                      \
		std::uint32_t r;                                                   \
		__asm__ volatile("fmov s1, %w1\n\t" insn " s0, s1\n\tfmov %w0, s0" \
				 : "=r"(r)                                                 \
				 : "r"(a)                                                  \
				 : "v0", "v1");                                            \
		return r;                                                          \
	}                                                                      \
	[[nodiscard]] inline std::uint64_t name(std::uint64_t a) noexcept      \
	{                                                                      \
		std::uint64_t r;                                                   \
		__asm__ volatile("fmov d1, %1\n\t" insn " d0, d1\n\tfmov %0, d0"   \
				 : "=r"(r)                                                 \
				 : "r"(a)                                                  \
				 : "v0", "v1");                                            \
		return r;                                                          \
	}

UML_A32_FP_BINARY(add, "fadd")
UML_A32_FP_BINARY(sub, "fsub")
UML_A32_FP_BINARY(mul, "fmul")
UML_A32_FP_BINARY(div, "fdiv")
UML_A32_FP_BINARY(max, "fmax")
UML_A32_FP_BINARY(min, "fmin")
UML_A32_FP_BINARY(max_number, "fmaxnm")
UML_A32_FP_BINARY(min_number, "fminnm")
UML_A32_FP_BINARY(abd, "fabd")
UML_A32_FP_BINARY(recip_step, "frecps")
UML_A32_FP_BINARY(rsqrt_step, "frsqrts")
/* The comparison masks: all ones when true. */
UML_A32_FP_BINARY(compare_eq, "fcmeq")
UML_A32_FP_BINARY(compare_ge, "fcmge")
UML_A32_FP_BINARY(compare_gt, "fcmgt")
UML_A32_FP_BINARY(abs_compare_ge, "facge")
UML_A32_FP_BINARY(abs_compare_gt, "facgt")

UML_A32_FP_UNARY(sqrt, "fsqrt")
UML_A32_FP_UNARY(recip_estimate, "frecpe")
UML_A32_FP_UNARY(rsqrt_estimate, "frsqrte")

#undef UML_A32_FP_BINARY
#undef UML_A32_FP_UNARY

/* The fused multiply-adds: addend + a * b (FMADD with the addend last). */
[[nodiscard]] inline std::uint32_t fused(std::uint32_t addend, std::uint32_t a,
	std::uint32_t b) noexcept
{
	std::uint32_t r;
	__asm__ volatile("fmov s1, %w1\n\tfmov s2, %w2\n\tfmov s3, %w3\n\t"
			 "fmadd s0, s2, s3, s1\n\tfmov %w0, s0"
			 : "=r"(r)
			 : "r"(addend), "r"(a), "r"(b)
			 : "v0", "v1", "v2", "s3");
	return r;
}
[[nodiscard]] inline std::uint64_t fused(std::uint64_t addend, std::uint64_t a,
	std::uint64_t b) noexcept
{
	std::uint64_t r;
	__asm__ volatile("fmov d1, %1\n\tfmov d2, %2\n\tfmov d3, %3\n\t"
			 "fmadd d0, d2, d3, d1\n\tfmov %0, d0"
			 : "=r"(r)
			 : "r"(addend), "r"(a), "r"(b)
			 : "v0", "v1", "v2", "d3");
	return r;
}

/* FCMP / FCMPE: the NZCV result in bits 31:28. */
[[nodiscard]] inline std::uint32_t compare_flags(std::uint32_t a, std::uint32_t b,
	bool signal_quiet_nan) noexcept
{
	std::uint64_t flags;
	if (signal_quiet_nan)
		__asm__ volatile("fmov s1, %w1\n\tfmov s2, %w2\n\tfcmpe s1, s2\n\tmrs %0, nzcv"
				 : "=r"(flags) : "r"(a), "r"(b) : "v1", "v2", "cc");
	else
		__asm__ volatile("fmov s1, %w1\n\tfmov s2, %w2\n\tfcmp s1, s2\n\tmrs %0, nzcv"
				 : "=r"(flags) : "r"(a), "r"(b) : "v1", "v2", "cc");
	return static_cast<std::uint32_t>(flags) & 0xf000'0000U;
}
[[nodiscard]] inline std::uint32_t compare_flags(std::uint64_t a, std::uint64_t b,
	bool signal_quiet_nan) noexcept
{
	std::uint64_t flags;
	if (signal_quiet_nan)
		__asm__ volatile("fmov d1, %1\n\tfmov d2, %2\n\tfcmpe d1, d2\n\tmrs %0, nzcv"
				 : "=r"(flags) : "r"(a), "r"(b) : "v1", "v2", "cc");
	else
		__asm__ volatile("fmov d1, %1\n\tfmov d2, %2\n\tfcmp d1, d2\n\tmrs %0, nzcv"
				 : "=r"(flags) : "r"(a), "r"(b) : "v1", "v2", "cc");
	return static_cast<std::uint32_t>(flags) & 0xf000'0000U;
}


/* Which FPCR a floating-point form runs under. */
struct Standard final {};
struct Effective final {};
inline constexpr Standard standard{};
inline constexpr Effective effective{};

/* A function of the n, m and old d 32-bit elements of each D register of an
 * Advanced SIMD form, run under the Standard FPSCR value. */
template<typename Function>
inline bool lanes32(Exec &x, Standard, Function &&function)
{
	return run_standard(x.cpu, [&]() -> bool {
		return for_each_register(x, [&](std::uint64_t n, std::uint64_t m, std::uint64_t d) -> std::uint64_t {
			std::uint64_t out = 0;
			for (unsigned half = 0; half < 2U; ++half) {
				const unsigned shift = 32U * half;
				out |= std::uint64_t{function(static_cast<std::uint32_t>(n >> shift),
					       static_cast<std::uint32_t>(m >> shift),
					       static_cast<std::uint32_t>(d >> shift))}
					<< shift;
			}
			return out;
		});
	});
}

} // namespace uml::cpu::arm64::aarch32::detail::fp
