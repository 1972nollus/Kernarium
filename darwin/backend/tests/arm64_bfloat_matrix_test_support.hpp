// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_bfloat_dot_test_support.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace arm64_bfloat_matrix_test {

namespace dot = arm64_bfloat_dot_test;
namespace shared = dot::shared;
namespace arm64 = shared::arm64;
namespace engine_tests = shared::engine_tests;

using Registers = dot::Registers;
inline constexpr auto alias_cases = dot::alias_cases;

[[nodiscard]] constexpr std::uint32_t instruction(
	Registers registers = {0U, 1U, 2U}) noexcept
{
	return 0x6e40'ec00U |
		((registers.second & 31U) << 16U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, Registers registers)
{
	const auto addends = initial.vectors[registers.destination].bytes;
	const auto left = initial.vectors[registers.first].bytes;
	const auto right = initial.vectors[registers.second].bytes;
	auto expected = initial;
	std::array<std::byte, 16> result{};
	for (unsigned row = 0U; row < 2U; ++row) {
		for (unsigned column = 0U; column < 2U; ++column) {
			const unsigned lane = row * 2U + column;
			auto sum = shared::read_single(addends, lane);
			for (unsigned pair = 0U; pair < 2U; ++pair) {
				const unsigned left_element =
					row * 4U + pair * 2U;
				const unsigned right_element =
					column * 4U + pair * 2U;
				sum = dot::dot_add(sum,
					shared::read_bfloat(left, left_element),
					shared::read_bfloat(left,
						left_element + 1U),
					shared::read_bfloat(right,
						right_element),
					shared::read_bfloat(right,
						right_element + 1U),
					initial.fpcr);
			}
			shared::write_single(result, lane, sum);
		}
	}
	expected.vectors[registers.destination].bytes = result;
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements(
	const arm64::CpuState &initial)
{
	return shared::native_requirements(initial.fpcr,
		(initial.fpcr & shared::fpcr_ebf) != 0U);
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, Registers registers,
	const arm64::CpuState &initial, shared::ExpectedCounts &counts)
{
	const auto native = native_requirements(initial);
	harness.check_one(name, instruction(registers), initial,
		expected_state(initial, registers), native);
	counts.add(native);
}

} // namespace arm64_bfloat_matrix_test
