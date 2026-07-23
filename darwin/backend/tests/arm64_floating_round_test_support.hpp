// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_basic_floating_test_support.hpp"
#include "cpu/arm64/detail/floating_round.hpp"

#include <array>
#include <cstdint>

namespace arm64_floating_round_test {

namespace basic = arm64_basic_floating_test;
namespace common = arm64_simd_test;
namespace arm64 = uml::backend::arm64;
namespace detail = uml::backend::arm64::detail;

using Operation = detail::FloatingRoundOperation;

struct Form final {
	Operation operation;
	bool scalar;
	unsigned width;
	unsigned vector_bytes;
};

struct Registers final {
	unsigned destination;
	unsigned source;
};

inline constexpr std::array standard_operations{
	Operation::nearest, Operation::plus, Operation::minus, Operation::zero,
	Operation::ties_away, Operation::exact_current, Operation::current};
inline constexpr std::array sized_operations{
	Operation::int32_exact, Operation::int32_zero,
	Operation::int64_exact, Operation::int64_zero};
inline constexpr std::array register_cases{
	Registers{0, 1}, Registers{1, 1}, Registers{31, 30}, Registers{30, 31}};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto operation : standard_operations) {
		for (const auto width : basic::widths)
			function(Form{operation, true, width, 16U});
		for (const auto shape : basic::vector_shapes)
			function(Form{operation, false, shape[0], shape[1]});
	}
	for (const auto operation : sized_operations) {
		for (const auto width : {32U, 64U})
			function(Form{operation, true, width, 16U});
		for (const auto shape : {std::array{32U, 8U},
			std::array{32U, 16U}, std::array{64U, 16U}})
			function(Form{operation, false, shape[0], shape[1]});
	}
}

[[nodiscard]] constexpr unsigned operation_index(Operation operation) noexcept
{
	switch (operation) {
	case Operation::nearest: return 0;
	case Operation::plus: return 1;
	case Operation::minus: return 2;
	case Operation::zero: return 3;
	case Operation::ties_away: return 4;
	case Operation::exact_current: return 5;
	case Operation::current: return 6;
	case Operation::int32_exact: return 0;
	case Operation::int32_zero: return 1;
	case Operation::int64_exact: return 2;
	case Operation::int64_zero: return 3;
	}
	return 0;
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1}) noexcept
{
	constexpr std::array scalar_standard{
		0x1e24'4000U, 0x1e24'c000U, 0x1e25'4000U,
		0x1e25'c000U, 0x1e26'4000U, 0x1e27'4000U,
		0x1e27'c000U};
	constexpr std::array scalar_sized{
		0x1e28'c000U, 0x1e28'4000U,
		0x1e29'c000U, 0x1e29'4000U};
	constexpr std::array vector_half{
		0x0e79'8800U, 0x0ef9'8800U, 0x0e79'9800U,
		0x0ef9'9800U, 0x2e79'8800U, 0x2e79'9800U,
		0x2ef9'9800U};
	constexpr std::array vector_ordinary{
		0x0e21'8800U, 0x0ea1'8800U, 0x0e21'9800U,
		0x0ea1'9800U, 0x2e21'8800U, 0x2e21'9800U,
		0x2ea1'9800U};
	constexpr std::array vector_sized{
		0x2e21'e800U, 0x0e21'e800U,
		0x2e21'f800U, 0x0e21'f800U};
	const bool sized = detail::is_sized_round(form.operation);
	std::uint32_t value;
	if (form.scalar) {
		value = sized ? scalar_sized[operation_index(form.operation)] :
			scalar_standard[operation_index(form.operation)];
		value |= basic::scalar_type(form.width) << 22;
	} else {
		const bool q = form.vector_bytes == 16U;
		if (sized)
			value = vector_sized[operation_index(form.operation)];
		else if (form.width == 16U)
			value = vector_half[operation_index(form.operation)];
		else
			value = vector_ordinary[operation_index(form.operation)];
		value |= q ? 0x4000'0000U : 0U;
		value |= form.width == 64U ? 0x0040'0000U : 0U;
	}
	return value | ((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

template<typename Floating>
[[nodiscard]] Floating input_value(Operation operation)
{
	switch (operation) {
	case Operation::nearest:
	case Operation::ties_away:
	case Operation::exact_current:
	case Operation::current:
	case Operation::int32_exact:
	case Operation::int64_exact:
		return static_cast<Floating>(2.5);
	case Operation::plus: return static_cast<Floating>(2.25);
	case Operation::minus: return static_cast<Floating>(-2.25);
	case Operation::zero:
	case Operation::int32_zero:
	case Operation::int64_zero:
		return static_cast<Floating>(-2.75);
	}
	__builtin_unreachable();
}

template<typename Floating>
[[nodiscard]] Floating expected_value(Operation operation)
{
	switch (operation) {
	case Operation::nearest:
	case Operation::exact_current:
	case Operation::current:
	case Operation::int32_exact:
	case Operation::int64_exact:
		return static_cast<Floating>(2.0);
	case Operation::plus:
	case Operation::ties_away:
		return static_cast<Floating>(3.0);
	case Operation::minus: return static_cast<Floating>(-3.0);
	case Operation::zero:
	case Operation::int32_zero:
	case Operation::int64_zero:
		return static_cast<Floating>(-2.0);
	}
	__builtin_unreachable();
}

template<typename Floating>
void prepare(arm64::CpuState &cpu, const Form &form, Registers registers)
{
	const unsigned lanes = form.scalar ? 1U :
		form.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; ++lane)
		basic::set_lane(cpu, registers.source, lane,
			input_value<Floating>(form.operation));
}

} // namespace arm64_floating_round_test
