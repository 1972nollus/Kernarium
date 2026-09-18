// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace arm64_simd_two_register_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Registers final {
	unsigned destination;
	unsigned source;
};

inline constexpr std::array target_opcodes{
	0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U,
	18U, 19U, 20U};

[[nodiscard]] constexpr std::uint32_t instruction(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode, Registers registers)
{
	return (scalar ? 0x5e20'0800U : 0x0e20'0800U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((opcode & 31U) << 12) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

/*
 * This is an independent transcription of the scalar and vector allocation
 * tables. Keep it separate from the production decoders: an invalid tuple is
 * required to stop before any architectural effect.
 */
[[nodiscard]] constexpr bool allocated(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode)
{
	if (scalar) {
		switch (opcode) {
		case 3:
		case 7:
			return true;
		case 8:
		case 9:
			return size == 3U;
		case 10:
			return !unsigned_group && size == 3U;
		case 11:
			return size == 3U;
		case 18:
			return unsigned_group && size < 3U;
		case 20:
			return size < 3U;
		default:
			return false;
		}
	}

	switch (opcode) {
	case 0:
		return unsigned_group ? size < 2U : size < 3U;
	case 1:
		return !unsigned_group && size == 0U;
	case 2:
	case 4:
	case 6:
		return size < 3U;
	case 3:
	case 7:
	case 8:
	case 9:
	case 11:
		return q || size < 3U;
	case 5:
		return unsigned_group ? size <= 1U : size == 0U;
	case 10:
		return !unsigned_group && (q || size < 3U);
	case 18:
	case 20:
		return size < 3U;
	case 19:
		return unsigned_group && size < 3U;
	default:
		return false;
	}
}

template<typename Opcodes, typename Callback>
void for_each_allocated_form(const Opcodes &opcodes, Callback &&callback)
{
	for (const auto opcode : opcodes) {
		for (const bool q : {false, true})
			for (const bool unsigned_group : {false, true})
				for (unsigned size = 0; size < 4U; ++size)
					if (allocated(false, q, unsigned_group,
					    size, opcode))
						callback(false, q, unsigned_group,
							size, opcode);
		for (const bool unsigned_group : {false, true})
			for (unsigned size = 0; size < 4U; ++size)
				if (allocated(true, false, unsigned_group,
				    size, opcode))
					callback(true, false, unsigned_group,
						size, opcode);
	}
}

} // namespace arm64_simd_two_register_test
