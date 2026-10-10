// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>

namespace uml::cpu::arm64::gadgets::a32 {

/* The AArch32 builder of the gadget engine: it translates A32 and T32 guest
 * code, decoded by the shared decoder (cpu/arm64/aarch32/decode.hpp), into
 * the same data-only streams the A64 Builder produces, so one executor, one
 * program cache and one execution boundary run both (executor.cpp).
 *
 * A translation is named by its key (aarch32/guest_key.hpp): the guest
 * address of a block is 1 << 63 | pc for A32 and 1 << 63 | 1 << 62 | pc << 1
 * for T32, and every instruction's key is the block's plus four for each
 * guest unit before it (a word for A32, a halfword for T32). Every PC a
 * stream stores, and every BlockLink it follows, is such a key.
 *
 * The PC of a program as the engine reports it on an exit is the key of the
 * instruction that did not retire, or of the next instruction for one that
 * did (SVC, BKPT excepted: aarch32/README.md). */

struct BuildInput final {
	/* The key the block starts at. */
	std::uint64_t key{};
	/* The guest bytes from the key's address: 32-bit words for A32 and
	 * 16-bit halfwords for T32, in guest (little-endian) order. A window
	 * that ends inside a T32 instruction stops the block before it. */
	std::span<const std::byte> code;
	/* ITSTATE at the first instruction: zero for any cached block. */
	aarch32::ItState it{};
	/* A single instruction, uncached: no links, and no IT block is
	 * required to complete. */
	bool single{};
};

struct BuildOutput final {
	BuildResult result;
	/* Guest units the translation read, the ones the cache validates it
	 * against: the instructions' and, for a block that stopped at an
	 * undefined instruction, that instruction's too. */
	std::size_t units{};
	/* ITSTATE after the last instruction of a single-instruction build. */
	aarch32::ItState next_it{};
};

/* The block-wide limit on translated instructions: an IT block that starts
 * below it always completes inside the block. */
[[nodiscard]] BuildOutput build(const BuildInput &input,
	const Builder::LinkResolver &resolve_link,
	std::pmr::memory_resource *storage,
	const Builder::IndirectLinkResolver &resolve_indirect_link,
	const BuildOptions &options, const aarch32::Profile &profile,
	std::size_t block_instructions);

/* Where each instruction of a T32 block starts, given the block's halfwords:
 * the number of instructions before the halfword `unit`, or nothing when
 * `unit` is not the start of an instruction. */
[[nodiscard]] std::optional<std::size_t> thumb_position(
	std::span<const std::byte> halfwords, std::size_t unit) noexcept;

/* ITSTATE at the instruction starting at `unit` of a T32 block that begins
 * with ITSTATE = 0, found by decoding the block's halfwords again. */
[[nodiscard]] std::uint8_t thumb_itstate(std::span<const std::byte> halfwords,
	std::size_t unit, const aarch32::Profile &profile) noexcept;

/* The operand word of a gadget that calls the tail helper
 * (tail_helpers.cpp): the operation in bits 0-7 and, one byte each, the
 * CpuState byte offsets of d, n, m, a, the high and low halves of a long
 * result, and an immediate. */
namespace tail {
inline constexpr unsigned op_shift = 0;
inline constexpr unsigned d_shift = 8;
inline constexpr unsigned n_shift = 16;
inline constexpr unsigned m_shift = 24;
inline constexpr unsigned a_shift = 32;
inline constexpr unsigned hi_shift = 40;
inline constexpr unsigned imm_shift = 48;
inline constexpr unsigned lo_shift = 56;
} // namespace tail

} // namespace uml::cpu::arm64::gadgets::a32
