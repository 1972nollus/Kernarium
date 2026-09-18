// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_system_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uml::emulator::tests::simd_lut {

namespace arm64 = uml::cpu::arm64;

inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

struct Form final {
	unsigned op2{};
	unsigned len{};
	unsigned op{};
	unsigned index_bits{};
	unsigned element_bytes{};
	unsigned part{};
	unsigned table_count{};
};

struct Decoded final {
	bool recognized{};
	bool valid{};
	Form form{};
};

inline constexpr std::array forms{
	Form{1U, 1U, 0U, 4U, 1U, 0U, 1U},
	Form{1U, 3U, 0U, 4U, 1U, 1U, 1U},
	Form{1U, 0U, 1U, 4U, 2U, 0U, 2U},
	Form{1U, 1U, 1U, 4U, 2U, 1U, 2U},
	Form{1U, 2U, 1U, 4U, 2U, 2U, 2U},
	Form{1U, 3U, 1U, 4U, 2U, 3U, 2U},
	Form{2U, 0U, 1U, 2U, 1U, 0U, 1U},
	Form{2U, 1U, 1U, 2U, 1U, 1U, 1U},
	Form{2U, 2U, 1U, 2U, 1U, 2U, 1U},
	Form{2U, 3U, 1U, 2U, 1U, 3U, 1U},
	Form{3U, 0U, 0U, 2U, 2U, 0U, 1U},
	Form{3U, 0U, 1U, 2U, 2U, 1U, 1U},
	Form{3U, 1U, 0U, 2U, 2U, 2U, 1U},
	Form{3U, 1U, 1U, 2U, 2U, 3U, 1U},
	Form{3U, 2U, 0U, 2U, 2U, 4U, 1U},
	Form{3U, 2U, 1U, 2U, 2U, 5U, 1U},
	Form{3U, 3U, 0U, 2U, 2U, 6U, 1U},
	Form{3U, 3U, 1U, 2U, 2U, 7U, 1U},
};

[[nodiscard]] constexpr std::uint32_t encode(
	unsigned op2, unsigned len, unsigned op, unsigned indices,
	unsigned table, unsigned destination) noexcept
{
	return 0x4e00'0000U | ((op2 & 3U) << 22U) |
		((indices & 31U) << 16U) | ((len & 3U) << 13U) |
		((op & 1U) << 12U) | ((table & 31U) << 5U) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode(
	const Form &form, unsigned indices, unsigned table,
	unsigned destination) noexcept
{
	return encode(form.op2, form.len, form.op, indices, table,
		destination);
}

[[nodiscard]] constexpr Decoded decode(std::uint32_t instruction) noexcept
{
	if ((instruction & 0xff20'8c00U) != 0x4e00'0000U ||
	    (instruction & 0x00c0'0000U) == 0U)
		return {};
	const unsigned op2 = (instruction >> 22U) & 3U;
	const unsigned len = (instruction >> 13U) & 3U;
	const unsigned op = (instruction >> 12U) & 1U;
	Form form{
		.op2 = op2,
		.len = len,
		.op = op,
	};
	if (op2 == 1U) {
		if (op == 0U && (len & 1U) == 0U)
			return {.recognized = true};
		form.index_bits = 4U;
		form.element_bytes = op == 0U ? 1U : 2U;
		form.part = op == 0U ? len >> 1U : len;
		form.table_count = op == 0U ? 1U : 2U;
	} else if (op2 == 2U) {
		if (op == 0U)
			return {.recognized = true};
		form.index_bits = 2U;
		form.element_bytes = 1U;
		form.part = len;
		form.table_count = 1U;
	} else {
		form.index_bits = 2U;
		form.element_bytes = 2U;
		form.part = (len << 1U) | op;
		form.table_count = 1U;
	}
	return {
		.recognized = true,
		.valid = true,
		.form = form,
	};
}

} // namespace uml::emulator::tests::simd_lut
