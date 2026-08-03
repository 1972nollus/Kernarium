// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_floating_horizontal_test_support.hpp"

namespace arm64_floating_absolute_minmax_test {

namespace horizontal = arm64_floating_horizontal_test;
namespace basic = horizontal::basic;
namespace common = horizontal::common;
namespace arm64 = horizontal::arm64;
namespace engine_tests = horizontal::engine_tests;

using Operation = horizontal::Operation;
using Registers = horizontal::Registers;
using ExpectedCounts = horizontal::ExpectedCounts;
inline constexpr auto register_cases = horizontal::register_cases;

struct Form final {
	Operation operation;
	unsigned width;
	unsigned vector_bytes;
};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto operation : {Operation::absolute_maximum,
		Operation::absolute_minimum})
		for (const auto shape : basic::vector_shapes)
			function(Form{operation, shape[0], shape[1]});
}

[[nodiscard]] constexpr horizontal::Form horizontal_form(
	const Form &form) noexcept
{
	return {form.operation, horizontal::Kind::vector_binary,
		form.width, form.vector_bytes};
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	constexpr std::array maximum{0x0ec0'1c00U, 0x4ec0'1c00U,
		0x0ea0'dc00U, 0x4ea0'dc00U, 0x4ee0'dc00U};
	constexpr std::array minimum{0x2ec0'1c00U, 0x6ec0'1c00U,
		0x2ea0'dc00U, 0x6ea0'dc00U, 0x6ee0'dc00U};
	const auto &table = form.operation == Operation::absolute_maximum ?
		maximum : minimum;
	return table[horizontal::vector_shape(
		form.width, form.vector_bytes)] |
		(registers.destination & 31U) |
		((registers.first & 31U) << 5) |
		((registers.second & 31U) << 16);
}

[[nodiscard]] inline auto native_requirements(const Form &form)
{
	return horizontal::native_requirements(horizontal_form(form));
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	return horizontal::state(seed);
}

inline void set_lane_bits(arm64::CpuState &cpu, unsigned reg,
	unsigned lane, unsigned width, std::uint64_t bits)
{
	horizontal::set_lane_bits(cpu, reg, lane, width, bits);
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, const Form &form, Registers registers,
	const arm64::CpuState &initial, ExpectedCounts &counts)
{
	const auto converted = horizontal_form(form);
	const auto native = native_requirements(form);
	arm64::CpuState expected;
	if (form.width == 16U)
		expected = horizontal::expected_minmax<std::uint16_t>(
			initial, converted, registers);
	else if (form.width == 32U)
		expected = horizontal::expected_minmax<std::uint32_t>(
			initial, converted, registers);
	else
		expected = horizontal::expected_minmax<std::uint64_t>(
			initial, converted, registers);
	harness.check_one(name, instruction(form, registers), initial,
		expected, native);
	counts.add(native);
}

inline void require_counts(const engine_tests::EngineHarness &harness,
	const ExpectedCounts &expected)
{
	horizontal::require_counts(harness, expected);
}

} // namespace arm64_floating_absolute_minmax_test
