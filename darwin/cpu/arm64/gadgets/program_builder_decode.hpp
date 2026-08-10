// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "program.hpp"
#include "simd_operand_histogram.hpp"

#include "cpu/arm64/cpu_state.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace uml::cpu::arm64::gadgets::builder_detail {

/* The byte offset of a guest vector register inside CpuState, which every
 * SIMD and floating-point operand word in the decode fragments is built from.
 *
 * This was written out by hand in twenty-seven separate places, each one
 * redeclaring the same lambda. Beyond the duplication, that left the engine
 * with no single point where a guest vector operand is named, so G-4 could not
 * measure which registers translated code actually uses without editing every
 * copy - and G129 reverted an attempt that hooked the two helpers which do have
 * a shared form and captured almost nothing. One helper gives the measurement a
 * choke point and the bank a place to consult a mapping.
 */
[[nodiscard]] inline std::size_t vector_operand_offset(unsigned index) noexcept
{
	diagnostics::record_simd_operand(index);
	return offsetof(CpuState, vectors) + index * sizeof(VectorRegister);
}

static_assert(offsetof(CpuState, vectors) + 31U * sizeof(VectorRegister) <=
	0xffffU, "a vector operand offset must fit the 16-bit operand field");

struct DecodeContext final {
	std::uint64_t guest_address;
	std::uint64_t address;
	std::uint32_t instruction;
	/* The instructions after this one, for the decoders that fuse a pair into
	 * a single entry. Empty at the end of the window, so a decoder must check
	 * before reading it, and a fusing decoder owns advancing `compiled` past
	 * everything it consumed. */
	std::span<const std::uint32_t> following;
	std::size_t &compiled;
	bool &program_accesses_fp_state;
	std::pmr::vector<std::uintptr_t> &words;
	const Builder::LinkResolver &resolve_link;
	const Builder::IndirectLinkResolver &resolve_indirect_link;
	const BuildOptions &options;
};

enum class DecodeResult : std::uint8_t {
	decoded,
	terminal,
	implementation_gap,
};

[[nodiscard]] DecodeResult implementation_gap(DecodeContext &context);
[[nodiscard]] DecodeResult decode_branch_system(DecodeContext &context);
[[nodiscard]] DecodeResult decode_data_processing(DecodeContext &context);
[[nodiscard]] DecodeResult decode_floating_simd(DecodeContext &context);
[[nodiscard]] DecodeResult decode_load_store(DecodeContext &context);
[[nodiscard]] DecodeResult decode_exception(DecodeContext &context);

} // namespace uml::cpu::arm64::gadgets::builder_detail
