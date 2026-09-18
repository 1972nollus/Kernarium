// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "uml_darwin_guest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace arm64_simd_structure_test {

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;

enum class AddressMode : unsigned {
	no_offset,
	post_immediate,
	post_register,
};

struct MultipleForm final {
	unsigned opcode;
	unsigned register_count;
	unsigned structure_elements;
	std::string_view mnemonic;
};

/*
 * These are the seven allocated opcode partitions from the shared decode in
 * the architecture specification.  structure_elements==1 is the LD1/ST1
 * contiguous-register family; the remaining forms are de/interleaving
 * structures.
 */
inline constexpr std::array multiple_forms{
	MultipleForm{7, 1, 1, "LD1/ST1 one-register"},
	MultipleForm{10, 2, 1, "LD1/ST1 two-register"},
	MultipleForm{6, 3, 1, "LD1/ST1 three-register"},
	MultipleForm{2, 4, 1, "LD1/ST1 four-register"},
	MultipleForm{8, 2, 2, "LD2/ST2"},
	MultipleForm{4, 3, 3, "LD3/ST3"},
	MultipleForm{0, 4, 4, "LD4/ST4"},
};

struct SingleForm final {
	bool valid{};
	bool replicate{};
	unsigned register_count{};
	std::size_t element_size{};
	std::size_t lane{};
};

[[nodiscard]] inline constexpr bool multiple_arrangement_valid(
	const MultipleForm &form, bool full_vector, unsigned size)
{
	return form.structure_elements == 1U || full_vector || size != 3U;
}

[[nodiscard]] inline constexpr std::uint32_t multiple_instruction(
	bool full_vector, bool load, bool post_index, unsigned offset_register,
	unsigned opcode, unsigned size, unsigned rn, unsigned rt)
{
	return 0x0c00'0000U | (static_cast<unsigned>(full_vector) << 30) |
		(static_cast<unsigned>(post_index) << 23) |
		(static_cast<unsigned>(load) << 22) |
		((offset_register & 31U) << 16) | ((opcode & 15U) << 12) |
		((size & 3U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint32_t single_instruction(
	bool full_vector, bool load, bool post_index, unsigned extra_register,
	unsigned offset_register, unsigned opcode, unsigned scale_bit,
	unsigned size, unsigned rn, unsigned rt)
{
	return 0x0d00'0000U | (static_cast<unsigned>(full_vector) << 30) |
		(static_cast<unsigned>(post_index) << 23) |
		(static_cast<unsigned>(load) << 22) |
		((extra_register & 1U) << 21) |
		((offset_register & 31U) << 16) | ((opcode & 7U) << 13) |
		((scale_bit & 1U) << 12) | ((size & 3U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

/*
 * Independent transcription of the shared single-structure decode:
 *   scale=00: byte lane, index Q:S:size
 *   scale=01: halfword lane, size<0> must be zero
 *   scale=10: word/doubleword lane, with the specified reserved cells
 *   scale=11: load-and-replicate only, S must be zero
 */
[[nodiscard]] inline constexpr SingleForm decode_single(
	bool full_vector, bool load, unsigned extra_register, unsigned opcode,
	unsigned scale_bit, unsigned size)
{
	const unsigned scale = opcode >> 1;
	const unsigned count = (opcode & 1U) != 0U ?
		3U + extra_register : 1U + extra_register;
	if (scale == 3U) {
		if (!load || scale_bit != 0U)
			return {};
		return {
			.valid = true,
			.replicate = true,
			.register_count = opcode == 6U ?
				1U + extra_register : 3U + extra_register,
			.element_size = std::size_t{1} << size,
		};
	}
	if (scale == 0U)
		return {
			.valid = true,
			.register_count = count,
			.element_size = 1,
			.lane = (static_cast<std::size_t>(full_vector) << 3) |
				(scale_bit << 2) | size,
		};
	if (scale == 1U) {
		if ((size & 1U) != 0U)
			return {};
		return {
			.valid = true,
			.register_count = count,
			.element_size = 2,
			.lane = (static_cast<std::size_t>(full_vector) << 2) |
				(scale_bit << 1) | (size >> 1),
		};
	}
	if ((size & 2U) != 0U ||
	    ((size & 1U) != 0U && scale_bit != 0U))
		return {};
	return {
		.valid = true,
		.register_count = count,
		.element_size = (size & 1U) != 0U ? 8U : 4U,
		.lane = (size & 1U) != 0U ?
			static_cast<std::size_t>(full_vector) :
			(static_cast<std::size_t>(full_vector) << 1) | scale_bit,
	};
}

[[nodiscard]] inline constexpr bool post_index(AddressMode mode)
{
	return mode != AddressMode::no_offset;
}

[[nodiscard]] inline constexpr unsigned encoded_offset_register(
	AddressMode mode, unsigned register_offset)
{
	switch (mode) {
	case AddressMode::no_offset:
		return 0;
	case AddressMode::post_immediate:
		return 31;
	case AddressMode::post_register:
		return register_offset;
	}
	return 0;
}

} // namespace arm64_simd_structure_test
