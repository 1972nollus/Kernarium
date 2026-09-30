// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/decoder.hpp"
#include "memory/guest_address_space.hpp"
#include "program.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::x86_64::gadgets {

/* Exceptions the engine does not check for, because a guest that has not
 * asked for them never sees them: #MF while the x87 control word unmasks an
 * exception, #XM while MXCSR does, and #AC while RFLAGS.AC is set. While
 * any is enabled the runtime runs the instructions that could raise one
 * singly, through `execute_checked`. */
enum CheckedException : unsigned {
	check_x87 = 1U << 0U,
	check_simd = 1U << 1U,
	check_alignment = 1U << 2U,
};

inline constexpr std::uint64_t rflags_alignment_check = 1ULL << 18U;
/* The status word's error summary and busy bits, which x87 sets together
 * while an unmasked exception is pending. */
inline constexpr std::uint16_t x87_status_pending = 0x8080U;
inline constexpr std::uint16_t x87_exception_flags = 0x3fU;
inline constexpr std::uint32_t mxcsr_exception_masks = 0x1f80U;

[[nodiscard]] constexpr unsigned checked_exceptions(
	const CpuState &cpu) noexcept
{
	return ((cpu.x87_control & x87_exception_flags) != x87_exception_flags ?
			unsigned{check_x87} : 0U) |
		((cpu.mxcsr & mxcsr_exception_masks) != mxcsr_exception_masks ?
			unsigned{check_simd} : 0U) |
		((cpu.rflags & rflags_alignment_check) != 0 ?
			unsigned{check_alignment} : 0U);
}

/* The unmasked x87 exceptions pending in the status word. */
[[nodiscard]] constexpr unsigned x87_pending(const CpuState &cpu) noexcept
{
	return static_cast<unsigned>(cpu.x87_status & ~cpu.x87_control &
		x87_exception_flags);
}

/* Whether `instruction` could raise an exception `checked` enables, so that
 * it must run singly under `execute_checked`. */
[[nodiscard]] bool needs_check(const Instruction &instruction,
	unsigned checked) noexcept;

/* An x87 instruction that waits: it raises #MF first when an unmasked x87
 * exception is pending. FWAIT and MMX instructions wait too; FNINIT,
 * FNCLEX, FNSTSW, FNSTCW, FNSTENV and FNSAVE do not. */
[[nodiscard]] bool waits_for_x87(const Instruction &instruction) noexcept;

/* FLDENV, FLDCW and FRSTOR, which load the control word and may unmask a
 * pending exception rather than raise one. */
[[nodiscard]] bool x87_loads_state(const Instruction &instruction) noexcept;

/* An x87 instruction that computes, and may raise an exception of its own. */
[[nodiscard]] bool x87_computes(const Instruction &instruction) noexcept;

/* The bytes an x87 instruction stores to its memory operand, or zero. */
[[nodiscard]] std::size_t x87_store_bytes(
	const Instruction &instruction) noexcept;

/* A SIMD floating-point instruction, one that MXCSR's masks govern. */
[[nodiscard]] bool simd_floating(const Instruction &instruction) noexcept;

/* The linear address the instruction's memory operand names, as the
 * engine's address generation forms it. */
[[nodiscard]] std::uint64_t effective_address(const CpuState &cpu,
	const Gadget &gadget, const Operand &operand) noexcept;

/* Whether the instruction's first data access is misaligned for its size,
 * which raises #AC while RFLAGS.AC is set. */
[[nodiscard]] bool misaligned_access(const CpuState &cpu,
	const Gadget &gadget) noexcept;

/* The host's cumulative floating-point exception flags, as the MXCSR flags
 * they correspond to. */
void clear_host_floating_exceptions() noexcept;
[[nodiscard]] std::uint32_t host_floating_exceptions() noexcept;

} // namespace uml::cpu::x86_64::gadgets
