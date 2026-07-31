// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
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
namespace engine_tests = uml::backend::tests;

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

struct FormatBits final {
	unsigned width;
	std::uint64_t sign;
	std::uint64_t positive_zero;
	std::uint64_t negative_zero;
	std::uint64_t half;
	std::uint64_t one;
	std::uint64_t one_next;
	std::uint64_t one_previous;
	std::uint64_t half_ulp;
	std::uint64_t quarter_ulp;
	std::uint64_t one_and_half;
	std::uint64_t two;
	std::uint64_t two_and_quarter;
	std::uint64_t two_and_half;
	std::uint64_t three;
	std::uint64_t three_and_quarter;
	std::uint64_t three_and_half;
	std::uint64_t four;
	std::uint64_t four_and_quarter;
	std::uint64_t five;
	std::uint64_t five_and_half;
	std::uint64_t seven_and_half;
	std::uint64_t nine;
	std::uint64_t positive_infinity;
	std::uint64_t maximum_finite;
	std::uint64_t minimum_normal;
	std::uint64_t minimum_subnormal;
	std::uint64_t quiet_nan;
	std::uint64_t signaling_nan;
	std::uint64_t default_nan;
};

inline constexpr FormatBits half_bits{
	16U, 0x8000U, 0x0000U, 0x8000U, 0x3800U, 0x3c00U, 0x3c01U,
	0x3bffU,
	0x1000U, 0x0c00U, 0x3e00U, 0x4000U, 0x4080U, 0x4100U,
	0x4200U, 0x4280U, 0x4300U, 0x4400U, 0x4440U, 0x4500U,
	0x4580U, 0x4780U, 0x4880U, 0x7c00U, 0x7bffU, 0x0400U,
	0x0001U, 0x7e55U, 0x7c55U, 0x7e00U};
inline constexpr FormatBits single_bits{
	32U, 0x8000'0000U, 0x0000'0000U, 0x8000'0000U, 0x3f00'0000U,
	0x3f80'0000U, 0x3f80'0001U, 0x3f7f'ffffU, 0x3380'0000U,
	0x3300'0000U,
	0x3fc0'0000U, 0x4000'0000U, 0x4010'0000U, 0x4020'0000U,
	0x4040'0000U, 0x4050'0000U, 0x4060'0000U, 0x4080'0000U,
	0x4088'0000U, 0x40a0'0000U, 0x40b0'0000U, 0x40f0'0000U,
	0x4110'0000U, 0x7f80'0000U, 0x7f7f'ffffU, 0x0080'0000U,
	0x0000'0001U, 0x7fc0'0055U, 0x7f80'0055U, 0x7fc0'0000U};
inline constexpr FormatBits double_bits{
	64U, 0x8000'0000'0000'0000ULL, 0x0000'0000'0000'0000ULL,
	0x8000'0000'0000'0000ULL, 0x3fe0'0000'0000'0000ULL,
	0x3ff0'0000'0000'0000ULL,
	0x3ff0'0000'0000'0001ULL, 0x3fef'ffff'ffff'ffffULL,
	0x3ca0'0000'0000'0000ULL, 0x3c90'0000'0000'0000ULL,
	0x3ff8'0000'0000'0000ULL, 0x4000'0000'0000'0000ULL,
	0x4002'0000'0000'0000ULL, 0x4004'0000'0000'0000ULL,
	0x4008'0000'0000'0000ULL, 0x400a'0000'0000'0000ULL,
	0x400c'0000'0000'0000ULL, 0x4010'0000'0000'0000ULL,
	0x4011'0000'0000'0000ULL, 0x4014'0000'0000'0000ULL,
	0x4016'0000'0000'0000ULL, 0x401e'0000'0000'0000ULL,
	0x4022'0000'0000'0000ULL, 0x7ff0'0000'0000'0000ULL,
	0x7fef'ffff'ffff'ffffULL, 0x0010'0000'0000'0000ULL,
	0x0000'0000'0000'0001ULL, 0x7ff8'0000'0000'0055ULL,
	0x7ff0'0000'0000'0055ULL, 0x7ff8'0000'0000'0000ULL};

inline constexpr std::uint32_t fpsr_invalid = 1U << 0;
inline constexpr std::uint32_t fpsr_divide_by_zero = 1U << 1;
inline constexpr std::uint32_t fpsr_overflow = 1U << 2;
inline constexpr std::uint32_t fpsr_underflow = 1U << 3;
inline constexpr std::uint32_t fpsr_inexact = 1U << 4;
inline constexpr std::uint32_t fpsr_input_denormal = 1U << 7;

[[nodiscard]] constexpr const FormatBits &format_bits(unsigned width) noexcept
{
	return width == 16U ? half_bits : width == 32U ? single_bits :
		double_bits;
}

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

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(const Form &form)
{
	using uml::backend::ArmFeature;
	if (form.scalar) {
		auto requirements = engine_tests::NativeRequirements::features(
			ArmFeature::floating_point);
		return form.width == 16U ?
			requirements | ArmFeature::fp16 : requirements;
	}
	auto requirements = engine_tests::NativeRequirements::features(
		ArmFeature::advanced_simd);
	return form.width == 16U ?
		requirements | ArmFeature::fp16 : requirements;
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	auto cpu = common::state(seed);
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	return cpu;
}

[[nodiscard]] inline std::uint64_t lane_bits_raw(
	const arm64::CpuState &cpu, unsigned reg, unsigned lane,
	unsigned width)
{
	return detail::decode_little_endian(
		std::span{cpu.vectors[reg].bytes}.subspan(
			lane * (width / 8U), width / 8U));
}

inline void set_lane_bits(arm64::CpuState &cpu, unsigned reg, unsigned lane,
	unsigned width, std::uint64_t bits)
{
	detail::encode_little_endian(bits,
		std::span{cpu.vectors[reg].bytes}.subspan(
			lane * (width / 8U), width / 8U));
}

[[nodiscard]] constexpr unsigned active_lanes(const Form &form) noexcept
{
	return form.scalar ? 1U : form.vector_bytes / (form.width / 8U);
}

inline void fill_active_lanes(arm64::CpuState &cpu, unsigned reg,
	const Form &form, std::uint64_t bits)
{
	for (unsigned lane = 0; lane < active_lanes(form); ++lane)
		set_lane_bits(cpu, reg, lane, form.width, bits);
}

[[nodiscard]] inline arm64::CpuState expected_with_uniform_result(
	const arm64::CpuState &initial, const Form &form, Registers registers,
	std::uint64_t result_bits, std::uint32_t new_fpsr_flags = 0)
{
	auto expected = initial;
	std::array<std::byte, 16> result{};
	if (form.scalar && (initial.fpcr & 4U) != 0U) {
		result = form.operation == Operation::square_root ?
			initial.vectors[registers.destination].bytes :
			initial.vectors[registers.left].bytes;
	}
	for (unsigned lane = 0; lane < active_lanes(form); ++lane)
		detail::encode_little_endian(result_bits,
			std::span{result}.subspan(
				lane * (form.width / 8U), form.width / 8U));
	expected.vectors[registers.destination].bytes = result;
	expected.fpsr |= new_fpsr_flags;
	return expected;
}

[[nodiscard]] inline arm64::CpuState expected_with_lane_results(
	const arm64::CpuState &initial, const Form &form, Registers registers,
	std::span<const std::uint64_t> result_bits,
	std::uint32_t new_fpsr_flags = 0)
{
	auto expected = initial;
	std::array<std::byte, 16> result{};
	if (form.scalar && (initial.fpcr & 4U) != 0U) {
		result = form.operation == Operation::square_root ?
			initial.vectors[registers.destination].bytes :
			initial.vectors[registers.left].bytes;
	}
	for (unsigned lane = 0; lane < active_lanes(form); ++lane)
		detail::encode_little_endian(result_bits[lane],
			std::span{result}.subspan(
				lane * (form.width / 8U), form.width / 8U));
	expected.vectors[registers.destination].bytes = result;
	expected.fpsr |= new_fpsr_flags;
	return expected;
}

inline void prepare_uniform_inputs(arm64::CpuState &cpu, const Form &form,
	Registers registers, std::uint64_t left, std::uint64_t right)
{
	fill_active_lanes(cpu, registers.left, form, left);
	if (form.operation != Operation::square_root)
		fill_active_lanes(cpu, registers.right, form,
			registers.left == registers.right ? left : right);
}

[[nodiscard]] constexpr std::uint64_t negate_bits(
	const FormatBits &format, std::uint64_t bits) noexcept
{
	return bits ^ format.sign;
}

inline void prepare_exact(arm64::CpuState &cpu, const Form &form,
	Registers registers)
{
	const auto &format = format_bits(form.width);
	if (form.operation == Operation::square_root) {
		fill_active_lanes(cpu, registers.left, form, format.nine);
		return;
	}
	if (registers.left == registers.right) {
		fill_active_lanes(cpu, registers.left, form, format.two);
		return;
	}
	const auto left = form.operation == Operation::divide ?
		format.seven_and_half :
		form.operation == Operation::subtract ? format.five_and_half :
		format.one_and_half;
	const auto right = form.operation == Operation::divide ?
		format.two_and_half :
		form.operation == Operation::subtract ? format.two_and_quarter :
		format.two;
	prepare_uniform_inputs(cpu, form, registers, left, right);
}

[[nodiscard]] inline arm64::CpuState expected_exact(
	const arm64::CpuState &initial, const Form &form, Registers registers)
{
	const auto &format = format_bits(form.width);
	std::uint64_t result;
	if (form.operation == Operation::square_root)
		result = format.three;
	else if (registers.left == registers.right)
		switch (form.operation) {
		case Operation::multiply:
		case Operation::add: result = format.four; break;
		case Operation::divide: result = format.one; break;
		case Operation::subtract: result = format.positive_zero; break;
		case Operation::negative_multiply:
			result = negate_bits(format, format.four);
			break;
		case Operation::square_root: __builtin_unreachable();
		}
	else
		switch (form.operation) {
		case Operation::multiply: result = format.three; break;
		case Operation::divide: result = format.three; break;
		case Operation::add: result = format.three_and_half; break;
		case Operation::subtract: result = format.three_and_quarter; break;
		case Operation::negative_multiply:
			result = negate_bits(format, format.three);
			break;
		case Operation::square_root: result = format.three; break;
		}
	return expected_with_uniform_result(initial, form, registers, result);
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

} // namespace arm64_basic_floating_test
