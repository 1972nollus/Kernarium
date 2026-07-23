// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_basic_floating_test_support.hpp"
#include "cpu/arm64/detail/floating_reciprocal.hpp"

#include <array>
#include <cstdint>

namespace arm64_floating_reciprocal_test {

namespace basic = arm64_basic_floating_test;
namespace common = arm64_simd_test;
namespace arm64 = uml::backend::arm64;
namespace detail = uml::backend::arm64::detail;

using Operation = detail::FloatingReciprocalOperation;

struct Form final {
	Operation operation;
	bool scalar;
	unsigned width;
	unsigned vector_bytes;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array register_cases{
	Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
	Registers{31, 30, 29}, Registers{30, 31, 30}};

[[nodiscard]] constexpr bool unary(Operation operation) noexcept
{
	return operation != Operation::reciprocal_step &&
		operation != Operation::reciprocal_sqrt_step;
}

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto operation : {Operation::reciprocal_estimate,
		Operation::reciprocal_step, Operation::reciprocal_exponent,
		Operation::reciprocal_sqrt_estimate,
		Operation::reciprocal_sqrt_step}) {
		for (const auto width : basic::widths)
			function(Form{operation, true, width, 16U});
		if (operation == Operation::reciprocal_exponent)
			continue;
		for (const auto shape : basic::vector_shapes)
			function(Form{operation, false, shape[0], shape[1]});
	}
}

[[nodiscard]] constexpr unsigned scalar_shape(unsigned width) noexcept
{
	return width == 16U ? 0U : width == 32U ? 1U : 2U;
}

[[nodiscard]] constexpr unsigned vector_shape(unsigned width,
	unsigned vector_bytes) noexcept
{
	return width == 16U ? (vector_bytes == 16U ? 1U : 0U) :
		width == 32U ? (vector_bytes == 16U ? 3U : 2U) : 4U;
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	constexpr std::array scalar{
		std::array{0x5ef9'd800U, 0x5ea1'd800U, 0x5ee1'd800U},
		std::array{0x5e40'3c00U, 0x5e20'fc00U, 0x5e60'fc00U},
		std::array{0x5ef9'f800U, 0x5ea1'f800U, 0x5ee1'f800U},
		std::array{0x7ef9'd800U, 0x7ea1'd800U, 0x7ee1'd800U},
		std::array{0x5ec0'3c00U, 0x5ea0'fc00U, 0x5ee0'fc00U}};
	constexpr std::array vector{
		std::array{0x0ef9'd800U, 0x4ef9'd800U, 0x0ea1'd800U,
			0x4ea1'd800U, 0x4ee1'd800U},
		std::array{0x0e40'3c00U, 0x4e40'3c00U, 0x0e20'fc00U,
			0x4e20'fc00U, 0x4e60'fc00U},
		std::array<std::uint32_t, 5>{},
		std::array{0x2ef9'd800U, 0x6ef9'd800U, 0x2ea1'd800U,
			0x6ea1'd800U, 0x6ee1'd800U},
		std::array{0x0ec0'3c00U, 0x4ec0'3c00U, 0x0ea0'fc00U,
			0x4ea0'fc00U, 0x4ee0'fc00U}};
	const unsigned operation = static_cast<unsigned>(form.operation);
	std::uint32_t value = form.scalar ?
		scalar[operation][scalar_shape(form.width)] :
		vector[operation][vector_shape(form.width, form.vector_bytes)];
	value |= registers.destination & 31U;
	value |= (registers.first & 31U) << 5;
	if (!unary(form.operation))
		value |= (registers.second & 31U) << 16;
	return value;
}

template<typename Floating>
[[nodiscard]] Floating first_input(Operation operation)
{
	return static_cast<Floating>(
		operation == Operation::reciprocal_sqrt_estimate ||
		operation == Operation::reciprocal_sqrt_step ? 4.0 : 2.0);
}

template<typename Floating>
[[nodiscard]] Floating second_input(Operation operation)
{
	return static_cast<Floating>(
		operation == Operation::reciprocal_sqrt_step ? 0.25 : 0.5);
}

template<typename Floating>
[[nodiscard]] Floating expected(Operation operation)
{
	return static_cast<Floating>(
		operation == Operation::reciprocal_step ||
		operation == Operation::reciprocal_sqrt_step ||
		operation == Operation::reciprocal_exponent ? 1.0 : 0.5);
}

template<typename Floating>
[[nodiscard]] basic::FloatingInteger<Floating> expected_bits(
	Operation operation)
{
	if (operation == Operation::reciprocal_estimate ||
	    operation == Operation::reciprocal_sqrt_estimate) {
		if constexpr (sizeof(Floating) == 2)
			return 0x37fcU;
		else if constexpr (sizeof(Floating) == 4)
			return 0x3eff'8000U;
		else
			return 0x3fdf'f000'0000'0000ULL;
	}
	return std::bit_cast<basic::FloatingInteger<Floating>>(
		expected<Floating>(operation));
}

} // namespace arm64_floating_reciprocal_test
