// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_test_support.hpp"
#include "cpu/arm64/detail/floating_arithmetic.hpp"
#include "cpu/arm64/detail/instruction_helpers.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace arm64_basic_floating_test {

namespace common = arm64_simd_test;
namespace arm64 = uml::backend::arm64;
namespace detail = uml::backend::arm64::detail;

using Operation = detail::BasicFloatingOperation;

struct Form final {
	Operation operation;
	bool scalar;
	unsigned width;
	unsigned vector_bytes;
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

inline constexpr std::array scalar_operations{
	Operation::multiply, Operation::divide, Operation::add,
	Operation::subtract, Operation::negative_multiply,
	Operation::square_root};
inline constexpr std::array vector_operations{
	Operation::multiply, Operation::divide, Operation::add,
	Operation::subtract, Operation::square_root};
inline constexpr std::array widths{16U, 32U, 64U};
inline constexpr std::array vector_shapes{
	std::array{16U, 8U}, std::array{16U, 16U},
	std::array{32U, 8U}, std::array{32U, 16U},
	std::array{64U, 16U}};
inline constexpr std::array register_cases{
	Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
	Registers{1, 1, 1}, Registers{31, 30, 29}, Registers{30, 30, 31},
};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto operation : scalar_operations)
		for (const auto width : widths)
			function(Form{operation, true, width, 16U});
	for (const auto operation : vector_operations)
		for (const auto shape : vector_shapes)
			function(Form{operation, false, shape[0], shape[1]});
}

[[nodiscard]] constexpr unsigned scalar_opcode(Operation operation) noexcept
{
	switch (operation) {
	case Operation::multiply: return 0;
	case Operation::divide: return 1;
	case Operation::add: return 2;
	case Operation::subtract: return 3;
	case Operation::negative_multiply: return 8;
	case Operation::square_root: return 0;
	}
	return 0;
}

[[nodiscard]] constexpr unsigned scalar_type(unsigned width) noexcept
{
	return width == 16U ? 3U : width == 32U ? 0U : 1U;
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	std::uint32_t value;
	if (form.scalar) {
		value = form.operation == Operation::square_root ? 0x1e21'c000U :
			0x1e20'0800U | (scalar_opcode(form.operation) << 12);
		value |= scalar_type(form.width) << 22;
	} else {
		const bool q = form.vector_bytes == 16U;
		const bool half = form.width == 16U;
		const bool is_double = form.width == 64U;
		if (half) {
			switch (form.operation) {
			case Operation::add: value = 0x0e40'1400U; break;
			case Operation::subtract: value = 0x0ec0'1400U; break;
			case Operation::multiply: value = 0x2e40'1c00U; break;
			case Operation::divide: value = 0x2e40'3c00U; break;
			case Operation::square_root: value = 0x2ef9'f800U; break;
			default: value = 0U; break;
			}
		} else {
			switch (form.operation) {
			case Operation::add: value = 0x0e20'd400U; break;
			case Operation::subtract: value = 0x0ea0'd400U; break;
			case Operation::multiply: value = 0x2e20'dc00U; break;
			case Operation::divide: value = 0x2e20'fc00U; break;
			case Operation::square_root: value = 0x2ea1'f800U; break;
			default: value = 0U; break;
			}
			value |= is_double ? 0x0040'0000U : 0U;
		}
		value |= q ? 0x4000'0000U : 0U;
	}
	return value |
		(form.operation == Operation::square_root ? 0U :
			(registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) | (registers.destination & 31U);
}

template<typename Floating>
using FloatingInteger = std::conditional_t<sizeof(Floating) == 2,
	std::uint16_t, std::conditional_t<sizeof(Floating) == 8,
		std::uint64_t, std::uint32_t>>;

template<typename Floating>
void set_lane(arm64::CpuState &cpu, unsigned reg, unsigned lane,
	Floating value)
{
	detail::encode_little_endian(
		std::bit_cast<FloatingInteger<Floating>>(value),
		std::span{cpu.vectors[reg].bytes}.subspan(
			lane * sizeof(Floating), sizeof(Floating)));
}

template<typename Floating>
[[nodiscard]] FloatingInteger<Floating> lane_bits(
	const arm64::CpuState &cpu, unsigned reg, unsigned lane)
{
	return static_cast<FloatingInteger<Floating>>(detail::decode_little_endian(
		std::span{cpu.vectors[reg].bytes}.subspan(
			lane * sizeof(Floating), sizeof(Floating))));
}

template<typename Floating>
[[nodiscard]] Floating exact_left(Operation operation, unsigned lane)
{
	const auto delta = static_cast<Floating>(lane % 2U);
	return operation == Operation::subtract ? static_cast<Floating>(5.5) + delta :
		operation == Operation::divide ? static_cast<Floating>(7.5) :
		operation == Operation::square_root ? static_cast<Floating>(9.0) :
		static_cast<Floating>(1.5) + delta;
}

template<typename Floating>
[[nodiscard]] Floating exact_right(Operation operation)
{
	return operation == Operation::subtract ? static_cast<Floating>(2.25) :
		operation == Operation::divide ? static_cast<Floating>(2.5) :
		static_cast<Floating>(2.0);
}

template<typename Floating>
[[nodiscard]] Floating exact_result(Operation operation, unsigned lane)
{
	const auto left = exact_left<Floating>(operation, lane);
	const auto right = exact_right<Floating>(operation);
	switch (operation) {
	case Operation::multiply: return left * right;
	case Operation::divide: return left / right;
	case Operation::add: return left + right;
	case Operation::subtract: return left - right;
	case Operation::negative_multiply: return -(left * right);
	case Operation::square_root: return static_cast<Floating>(3.0);
	}
	__builtin_unreachable();
}

template<typename Floating>
void prepare_exact(arm64::CpuState &cpu, const Form &form,
	Registers registers)
{
	const unsigned lanes = form.scalar ? 1U :
		form.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		set_lane(cpu, registers.left, lane,
			exact_left<Floating>(form.operation, lane));
		set_lane(cpu, registers.right, lane,
			exact_right<Floating>(form.operation));
	}
}

} // namespace arm64_basic_floating_test
