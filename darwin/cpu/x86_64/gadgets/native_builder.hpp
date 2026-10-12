// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "program.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uml::cpu::x86_64::gadgets {

/* Append the block's stream to `stream`, which the caller may hand back with
 * its capacity intact so a translation does not buy the storage again.
 * Returns the index of the first gadget the bank has no form for, leaving a
 * partial stream, or nothing when the whole block was appended.
 *
 * `program_begin` is the address of the block the stream runs as, which the
 * run's execution context names as its program_begin whatever part of the
 * block the stream holds: a baked memory access carries its position from it,
 * and its fault and overlap exits add it back. A stream built from some of a
 * block's instructions, a budget-limited replay or one checked instruction,
 * therefore takes the block's address and not its own first instruction's. */
[[nodiscard]] std::optional<std::size_t> append_native_stream(
	std::vector<std::uintptr_t> &stream, std::span<const Gadget> gadgets,
	std::uint64_t final_rip, ExecutionMode mode,
	std::uint64_t program_begin, ProgramLinkResolver links = {});

/* The instruction of `gadgets` whose words in `stream`, the block's stream,
 * include word `word`, or nothing when the stream is not theirs. */
[[nodiscard]] std::optional<std::size_t> instruction_at_word(
	std::span<const Gadget> gadgets, std::span<const std::uintptr_t> stream,
	std::size_t word, ExecutionMode mode, std::uint64_t program_begin);

[[nodiscard]] std::vector<std::uintptr_t> build_native_stream(
	std::span<const Gadget> gadgets, std::uint64_t final_rip,
	ExecutionMode mode, std::uint64_t program_begin,
	ProgramLinkResolver links = {});

/* Whether the instruction is a string operation with a REP, REPE or REPNE
 * prefix. */
[[nodiscard]] bool repeated_string(const Instruction &instruction) noexcept;

/* One iteration of a repeated string instruction, as a single step runs
 * it: the stream leaves between elements with RIP naming the instruction,
 * or retires it when the iteration ends it. A string instruction carries no
 * baked access word, so the stream has no position to take from its
 * block. */
[[nodiscard]] std::vector<std::uintptr_t> build_repeat_step_stream(
	const Gadget &gadget, ExecutionMode mode);

/* A stream that runs nothing and leaves with RIP at `rip`: a check head's,
 * whose instruction the runtime runs itself. */
void append_leave_stream(std::vector<std::uintptr_t> &stream,
	std::uint64_t rip);

} // namespace uml::cpu::x86_64::gadgets
