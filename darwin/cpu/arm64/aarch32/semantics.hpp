// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "memory/guest_address_space.hpp"

#include <cstddef>
#include <cstdint>

/*
 * The engine-neutral semantics of the AArch32 profile (design 3.2, lane
 * A32-S): one C++ implementation of every instruction the decoder accepts at
 * the library's level, on CpuState alone, for the engines' slow paths and
 * long tails and for the instruction corpus's model lane. It follows the SVE
 * library (detail/sve_semantics.hpp): it includes no engine header, and
 * reaches guest memory only through the caller's A32Memory.
 *
 * One call executes one decoded instruction, with the architecture's fault
 * ordering:
 *  - a store writes nothing until every page it touches translates, except
 *    that STM, PUSH, VSTM and VPUSH store the words ahead of the one that
 *    faults and leave the base register as it was (the pseudocode's order);
 *  - a load, LDM and POP included, writes its registers last, so a fault
 *    leaves them as they were;
 *  - a fault reports the 32-bit architectural address, and the registers,
 *    PC and ITSTATE stay the instruction's own.
 */

namespace uml::cpu::arm64::aarch32 {

/*
 * Guest memory as the caller's engine reaches it. translate() returns the
 * host address of the guest byte at `address` (a 32-bit address, zero
 * extended) for `access`, valid through the end of its guest page
 * (DARWIN_UML_GUEST_PAGE_SIZE), or nullptr when the page does not translate
 * that way; it need not record a fault, since the library reports the
 * address and the access itself and the engine records them where its own
 * exit convention wants them. After a store has moved bytes the library
 * reports each host range it wrote to stored(), so an engine can invalidate
 * the reservations and the translated code they overlap.
 *
 * The exclusives and the acquire/release forms use the host pointer of a
 * translation for atomic accesses, so the pages translate() returns must be
 * real, shared host memory.
 */
class A32Memory {
public:
	[[nodiscard]] virtual std::byte *translate(std::uint64_t address,
		::uml::memory::MemoryAccess access) noexcept = 0;
	virtual void stored(std::byte *host, std::size_t size) noexcept = 0;

protected:
	A32Memory() = default;
	~A32Memory() = default;
	A32Memory(const A32Memory &) = default;
	A32Memory &operator=(const A32Memory &) = default;
};

enum class StepEvent : std::uint8_t {
	/* The instruction completed. PC (the key) holds the next instruction or
	 * the branch target, and ITSTATE the state after the instruction. */
	retired,
	/* SVC. PC is past the instruction and ITSTATE advanced; the immediate is
	 * in `syndrome`. */
	supervisor_call,
	/* BKPT. PC and ITSTATE stay the instruction's; its 16-bit immediate is
	 * in `syndrome`. */
	breakpoint,
	/* The instruction is UNDEFINED: the decoder rejected it, or it is a UDF
	 * that executes. Nothing changes. */
	undefined,
	/* A data abort or alignment fault at `fault_address` for `access`.
	 * Nothing changes except what the header comment lists. step_a32()
	 * also reports an instruction fetch that does not translate this way,
	 * with access `execute`. */
	memory_fault,
	/* step_a32() only: the PC is an A32 address with bit 1 set, which is not
	 * a canonical key (guest_key.hpp). Nothing executes; `fault_address` is
	 * the PC. */
	pc_alignment,
	/* The instruction is outside the library's contract (a form it does not
	 * cover, or a Level the caller's decode did not apply): an internal
	 * error that changes nothing. */
	fatal,
};

struct StepResult final {
	StepEvent event{StepEvent::retired};
	/* For memory_fault: an alignment fault rather than a translation fault. */
	bool alignment{};
	::uml::memory::MemoryAccess access{::uml::memory::MemoryAccess::read};
	/* The 32-bit address of the first byte of the access that faulted. */
	std::uint32_t fault_address{};
	std::uint32_t syndrome{};
};

/*
 * Whether execute_a32() implements the form, which holds for every form the
 * library's level covers (a form above it executes as fatal). The corpus's
 * model lane counts a case whose forms are not covered as skipped.
 */
[[nodiscard]] bool a32_semantics_covers(Form form) noexcept;

/* The highest level the library implements. */
[[nodiscard]] constexpr Level a32_semantics_level() noexcept
{
	return Level::base;
}

/*
 * Executes `instruction`, which decode_a32() or decode_thumb() returned for
 * the word at the PC key in `cpu.pc`, in the ITSTATE in `cpu.a32_itstate`
 * (the state the decode was made in). The registers are CpuState's AArch32
 * mapping (R0-R14 in x0-x14 zero extended, NZCV in nzcv, Q in a32_q, GE in
 * a32_ge_mask, FPSCR in fpcr, fpsr and a32_fpscr_nzcv, D registers in
 * vectors). A decoded-undefined instruction is the `undefined` event; an
 * instruction whose condition fails retires with no other effect.
 *
 * A taken branch to an A32 target with bit 1 set leaves that target in the
 * PC key, which is not a canonical key: the engine's next fetch raises the
 * PC alignment exit (guest_key.hpp). The exclusive monitor is CpuState's
 * record (exclusive_address, exclusive_key, exclusive_value and
 * exclusive_size); a store-exclusive completes as a host compare-and-swap
 * against the value the load-exclusive recorded. Ordinary stores leave the
 * monitor alone; the engine clears it where its own model does.
 */
[[nodiscard]] StepResult execute_a32(CpuState &cpu,
	const Instruction &instruction, A32Memory &memory) noexcept;

/*
 * The single-instruction contract: fetches the instruction at the PC key in
 * `cpu.pc` through `memory` (access `execute`), decodes it against `profile`
 * in the ITSTATE in `cpu.a32_itstate`, and executes it. This is the entry
 * for what an engine cannot translate as a block (a mid-IT entry, a single
 * step) and the corpus's model lane's loop: calling it until ITSTATE is zero
 * leaves an IT block exactly as the architecture does, one instruction and
 * one ITAdvance at a time. A fetch that does not translate is a
 * `memory_fault` for `execute` at the first byte that failed (for a T32
 * instruction whose second halfword is on an unmapped page, the address of
 * that halfword), with the PC at the instruction.
 */
[[nodiscard]] StepResult step_a32(CpuState &cpu, A32Memory &memory,
	const Profile &profile) noexcept;

/*
 * Library-exit counters: with counting on, every execute_a32() call adds to
 * the count of the form it executed, so a test can require a form an engine
 * lowers to stay out of the library from a clean state, and an engine's
 * report can name the hot words that land here (the SVE counters'
 * counterpart).
 */
void count_a32_library_exits(bool enable) noexcept;
[[nodiscard]] std::uint64_t a32_library_exits(Form form) noexcept;
[[nodiscard]] std::uint64_t a32_library_exit_total() noexcept;
void reset_a32_library_exits() noexcept;

} // namespace uml::cpu::arm64::aarch32
