// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/decoder.hpp"
#include "memory/guest_address_space.hpp"
#include "program.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::x86_64::gadgets {

/* Exceptions a guest that has not asked for them never sees: #MF while the
 * x87 control word unmasks an exception, #XM while MXCSR does, and #AC while
 * RFLAGS.AC is set. The gadgets check x87 exceptions themselves while one is
 * unmasked, and SIMD ones while MXCSR masks underflow, precision and a
 * denormal operand (`gadget_checks`). For the others -- #AC, #XM while one of
 * those three is unmasked, and an x87 exception left pending for the next
 * waiting instruction -- the runtime runs the instructions that could raise
 * one singly, through `execute_checked` (`checked_exceptions`). */
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
/* Underflow, precision and a denormal operand: the gadgets cannot tell from
 * a value whether an instruction raised any of them. */
inline constexpr std::uint32_t mxcsr_unchecked_masks = 0x1900U;
inline constexpr std::uint32_t mxcsr_denormals_are_zero = 0x40U;
inline constexpr std::uint32_t mxcsr_flush_to_zero = 0x8000U;
inline constexpr std::uint32_t mxcsr_denormal_operand = 0x02U;

/* The unmasked x87 exceptions pending in the status word. */
[[nodiscard]] constexpr unsigned x87_pending(const CpuState &cpu) noexcept
{
	return static_cast<unsigned>(cpu.x87_status & ~cpu.x87_control &
		x87_exception_flags);
}

/* Every exception the guest enables: what an instruction run singly is
 * checked for. */
[[nodiscard]] constexpr unsigned enabled_exceptions(
	const CpuState &cpu) noexcept
{
	return ((cpu.x87_control & x87_exception_flags) != x87_exception_flags ?
			unsigned{check_x87} : 0U) |
		((cpu.mxcsr & mxcsr_exception_masks) != mxcsr_exception_masks ?
			unsigned{check_simd} : 0U) |
		((cpu.rflags & rflags_alignment_check) != 0 ?
			unsigned{check_alignment} : 0U);
}

/* The exceptions whose instructions the runtime runs singly: an enabled #AC,
 * #XM while underflow, precision or a denormal operand is unmasked, and #MF
 * while an unmasked x87 exception is pending, which every waiting
 * instruction raises before it runs. An instruction that raises another
 * checks it itself. */
[[nodiscard]] constexpr unsigned checked_exceptions(
	const CpuState &cpu) noexcept
{
	return (x87_pending(cpu) != 0 ? unsigned{check_x87} : 0U) |
		((cpu.mxcsr & mxcsr_unchecked_masks) != mxcsr_unchecked_masks ?
			unsigned{check_simd} : 0U) |
		(enabled_exceptions(cpu) & unsigned{check_alignment});
}

/* CpuState::exception_checks for an instruction the runtime runs checked:
 * the gadgets check nothing, and take their exact path while MXCSR.DAZ or
 * FTZ is set. */
[[nodiscard]] constexpr std::uint8_t checked_run_checks(
	const CpuState &cpu) noexcept
{
	return (cpu.mxcsr & (mxcsr_denormals_are_zero | mxcsr_flush_to_zero)) !=
			0U ?
		exception_check_simd_exact : std::uint8_t{};
}

/* What the gadgets check themselves, as CpuState::exception_checks. */
[[nodiscard]] constexpr std::uint8_t gadget_checks(
	const CpuState &cpu) noexcept
{
	const auto simd =
		(cpu.mxcsr & mxcsr_exception_masks) != mxcsr_exception_masks &&
		(cpu.mxcsr & mxcsr_unchecked_masks) == mxcsr_unchecked_masks;
	return static_cast<std::uint8_t>(
		((cpu.x87_control & x87_exception_flags) != x87_exception_flags ?
			exception_check_x87 : 0U) |
		(simd ? exception_check_simd | exception_check_simd_exact : 0U) |
		checked_run_checks(cpu));
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

/* An x87 comparison: one that sets C3, C2 and C0 (FCOM, FUCOM, FICOM,
 * FTST), or ZF, PF and CF (FCOMI, FUCOMI). */
enum class X87Compare : std::uint8_t {
	none,
	status,
	flags,
};
[[nodiscard]] X87Compare x87_compare(const Instruction &instruction) noexcept;

/* FLD of a single or a double, which completes beside an unmasked denormal
 * operand. */
[[nodiscard]] bool x87_converts_load(const Instruction &instruction) noexcept;

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

/* MXCSR.DE for the SIMD floating-point instruction `gadget`, from the state
 * before it runs, which the host's flags do not show: x86 raises it for a
 * denormal source in a lane it computes, unless MXCSR.DAZ makes the source
 * zero, or the lane holds a NaN or raises invalid operation or division by
 * zero, which take precedence. Rounding, RCP, RSQRT, VCVTPH2PS and
 * conversions to integers raise none, and DPPS and DPPD leave it to
 * `host_flag_correction`. Zero when a memory source is unreadable, which
 * the instruction then faults on. */
[[nodiscard]] std::uint32_t denormal_operand(const CpuState &cpu,
	const Gadget &gadget, ::uml::memory::GuestAddressSpace &memory);

/* MXCSR flags the host's floating-point operations raise differently from
 * x86 for an instruction run checked: the flags in `decided` take their
 * values from `raised` rather than from the host. `settled` flags are all
 * of x86's, the denormal operand and the precedence of an unmasked
 * exception included. */
struct HostFlagCorrection final {
	std::uint32_t decided{};
	std::uint32_t raised{};
	bool settled{};
};

/* The correction for the SIMD floating-point instruction `gadget`, from the
 * state before it runs. An FMA decides invalid operation, which the host
 * raises for infinity times zero plus a quiet NaN and x86 does not. A sum,
 * difference, product, quotient, FMA or narrowing to a single or half
 * decides underflow, which x86 detects after rounding and the host before,
 * and which x86 raises for an exact tiny result while underflow is unmasked
 * or FTZ flushes it, with precision then; beside an unmasked underflow or
 * overflow, it decides precision too, which x86 raises as the significand
 * rounds. ROUND and conversions to integers decide precision, which ROUND's
 * gadget raises in software, outside the host flags a checked run reads.
 * DPPS and DPPD settle their flags, which x86 raises step by step, faulting
 * at the first step that raises an unmasked exception. Nothing when a
 * memory source is unreadable, which the instruction then faults on. */
[[nodiscard]] HostFlagCorrection host_flag_correction(const CpuState &cpu,
	const Gadget &gadget, ::uml::memory::GuestAddressSpace &memory);

/* The host's cumulative floating-point exception flags, as the MXCSR flags
 * they correspond to. Outside a checked run they are the guest's: a run
 * clears them as it starts and adds them to MXCSR as it ends, STMXCSR,
 * FXSAVE and XSAVE add them to the MXCSR they store, and LDMXCSR, FXRSTOR
 * and XRSTOR clear them with the flags they load. */
void clear_host_floating_exceptions() noexcept;
[[nodiscard]] std::uint32_t host_floating_exceptions() noexcept;

} // namespace uml::cpu::x86_64::gadgets
