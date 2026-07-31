// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_simd_test_support.hpp"
#include "cpu/arm64/detail/instruction_helpers.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_simd_fp_indexed_test {

namespace arm64 = uml::backend::arm64;
namespace base = arm64_simd_test;
namespace engine_tests = uml::backend::tests;

enum class Operation : unsigned { fmla = 1, fmls = 5, fmul = 9, fmulx = 25 };

inline constexpr std::array operations{
	Operation::fmla, Operation::fmls, Operation::fmul, Operation::fmulx};

struct Shape final {
	unsigned size;
	unsigned lane_bytes;
	unsigned indexes;
	bool scalar;
	bool q;
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned element;
};

inline constexpr std::array shapes{
	Shape{0U, 2U, 8U, true, true}, Shape{0U, 2U, 8U, false, false},
	Shape{0U, 2U, 8U, false, true}, Shape{2U, 4U, 4U, true, true},
	Shape{2U, 4U, 4U, false, false}, Shape{2U, 4U, 4U, false, true},
	Shape{3U, 8U, 2U, true, true}, Shape{3U, 8U, 2U, false, true}};

inline constexpr std::uint32_t fpsr_invalid = 1U << 0;
inline constexpr std::uint32_t fpsr_overflow = 1U << 2;
inline constexpr std::uint32_t fpsr_underflow = 1U << 3;
inline constexpr std::uint32_t fpsr_inexact = 1U << 4;
inline constexpr std::uint32_t fpsr_input_denormal = 1U << 7;
inline constexpr std::uint32_t fpsr_saturation = 1U << 27;

[[nodiscard]] constexpr bool accumulating(Operation operation)
{
	return operation == Operation::fmla || operation == Operation::fmls;
}

[[nodiscard]] constexpr std::string_view operation_name(Operation operation)
{
	switch (operation) {
	case Operation::fmla: return "fmla";
	case Operation::fmls: return "fmls";
	case Operation::fmul: return "fmul";
	case Operation::fmulx: return "fmulx";
	}
	return "unknown";
}

[[nodiscard]] constexpr unsigned active_bytes(const Shape &shape)
{
	return shape.scalar ? shape.lane_bytes : shape.q ? 16U : 8U;
}

[[nodiscard]] constexpr unsigned element_registers(const Shape &shape)
{
	return shape.size == 0U ? 16U : 32U;
}

[[nodiscard]] constexpr std::uint32_t instruction(Operation operation,
	const Shape &shape, unsigned destination, unsigned left,
	unsigned element_register, unsigned index)
{
	const bool extended = operation == Operation::fmulx;
	const unsigned opcode = static_cast<unsigned>(operation) & 15U;
	std::uint32_t encoded = (shape.scalar ? 0x5f00'0000U : 0x0f00'0000U) |
		(!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
		(extended ? 0x2000'0000U : 0U) |
		((shape.size & 3U) << 22) | ((opcode & 15U) << 12) |
		((element_register & 15U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
	if (shape.size == 0U) {
		encoded |= (index & 1U) << 20;
		encoded |= (index & 2U) << 20;
		encoded |= (index & 4U) << 9;
	} else {
		encoded |= ((element_register >> 4) & 1U) << 20;
		if (shape.size == 2U) {
			encoded |= (index & 1U) << 21;
			encoded |= (index & 2U) << 10;
		} else {
			encoded |= (index & 1U) << 11;
		}
	}
	return encoded;
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	auto cpu = base::state(seed);
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	cpu.set_mte_configuration(true, 0xa55aU);
	cpu.set_mte_generator(true, 0x1357U, 9U,
		0x3141'5926'5358'9793ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0x89ab'cdefU;
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_sm = true;
	cpu.pstate_za = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xbU;
	cpu.exclusive_address = 0x1234'5000ULL;
	cpu.exclusive_key = 0x2468U;
	cpu.exclusive_value = 0x1122'3344'5566'7788ULL;
	cpu.exclusive_value2 = 0x8877'6655'4433'2211ULL;
	cpu.exclusive_version = 0x1020'3040'5060'7080ULL;
	cpu.exclusive_size = 8U;
	return cpu;
}

[[nodiscard]] inline std::uint64_t read_bits(
	const std::array<std::byte, 16> &bytes, unsigned lane_bytes,
	unsigned lane)
{
	return arm64::detail::decode_little_endian(
		std::span{bytes}.subspan(lane * lane_bytes, lane_bytes));
}

inline void write_bits(std::array<std::byte, 16> &bytes, unsigned lane_bytes,
	unsigned lane, std::uint64_t bits)
{
	arm64::detail::encode_little_endian(bits,
		std::span{bytes}.subspan(lane * lane_bytes, lane_bytes));
}

inline void fill_bits(std::array<std::byte, 16> &bytes, unsigned lane_bytes,
	std::uint64_t bits)
{
	for (unsigned lane = 0; lane < 16U / lane_bytes; ++lane)
		write_bits(bytes, lane_bytes, lane, bits);
}

[[nodiscard]] inline std::uint64_t floating_bits(unsigned lane_bytes,
	double value)
{
	if (lane_bytes == 2U)
		return std::bit_cast<std::uint16_t>(static_cast<_Float16>(value));
	if (lane_bytes == 4U)
		return std::bit_cast<std::uint32_t>(static_cast<float>(value));
	return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] inline long double floating_value(unsigned lane_bytes,
	std::uint64_t bits)
{
	if (lane_bytes == 2U)
		return std::bit_cast<_Float16>(static_cast<std::uint16_t>(bits));
	if (lane_bytes == 4U)
		return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
	return std::bit_cast<double>(bits);
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements(
	const Shape &shape, std::uint32_t fpcr = 0U)
{
	using uml::backend::ArmFeature;
	auto requirements = engine_tests::NativeRequirements::features(
		ArmFeature::advanced_simd);
	if (shape.lane_bytes == 2U)
		requirements = requirements | ArmFeature::fp16;
	if ((fpcr & 2U) != 0U ||
	    (shape.scalar && (fpcr & 4U) != 0U) ||
	    (shape.lane_bytes != 2U && (fpcr & 1U) != 0U))
		requirements = requirements | ArmFeature::afp;
	return requirements;
}

[[nodiscard]] inline arm64::CpuState expected_uniform(
	const arm64::CpuState &initial, const Shape &shape, Operation operation,
	unsigned destination, unsigned left, std::uint64_t result_bits,
	std::uint32_t raised_flags = 0U)
{
	auto expected = initial;
	std::array<std::byte, 16> result{};
	if (shape.scalar && (initial.fpcr & 4U) != 0U)
		result = accumulating(operation) ?
			initial.vectors[destination].bytes :
			initial.vectors[left].bytes;
	for (unsigned lane = 0;
	     lane < active_bytes(shape) / shape.lane_bytes; ++lane)
		write_bits(result, shape.lane_bytes, lane, result_bits);
	expected.vectors[destination].bytes = result;
	expected.fpsr |= raised_flags;
	return expected;
}

inline void check_uniform(engine_tests::EngineHarness &harness,
	std::string_view name, Operation operation, const Shape &shape,
	Registers registers, unsigned index, const arm64::CpuState &initial,
	std::uint64_t result_bits, std::uint32_t raised_flags = 0U)
{
	harness.check_one(name, instruction(operation, shape,
		registers.destination, registers.left, registers.element, index),
		initial, expected_uniform(initial, shape, operation,
			registers.destination, registers.left, result_bits,
			raised_flags),
		native_requirements(shape, initial.fpcr));
}

/*
 * The allocation oracle deliberately uses only small binary integers. Their
 * products, sums, and differences are exact in all three element formats, so
 * this calculation does not depend on the emulator's floating implementation
 * or on host rounding behavior.
 */
[[nodiscard]] inline std::uint64_t exact_result_bits(Operation operation,
	unsigned lane_bytes, std::uint64_t accumulator_bits,
	std::uint64_t left_bits, std::uint64_t element_bits)
{
	const auto accumulator = floating_value(lane_bytes, accumulator_bits);
	const auto left = floating_value(lane_bytes, left_bits);
	const auto element = floating_value(lane_bytes, element_bits);
	long double result = 0.0;
	switch (operation) {
	case Operation::fmla:
		result = accumulator + left * element;
		break;
	case Operation::fmls:
		result = accumulator - left * element;
		break;
	case Operation::fmul:
	case Operation::fmulx:
		result = left * element;
		break;
	}
	return floating_bits(lane_bytes, static_cast<double>(result));
}

[[nodiscard]] inline arm64::CpuState expected_exact(
	const arm64::CpuState &initial, Operation operation, const Shape &shape,
	Registers registers, unsigned index)
{
	const auto accumulator =
		initial.vectors[registers.destination].bytes;
	const auto left = initial.vectors[registers.left].bytes;
	const auto element = initial.vectors[registers.element].bytes;
	std::array<std::byte, 16> result{};
	if (shape.scalar && (initial.fpcr & 4U) != 0U)
		result = accumulating(operation) ? accumulator : left;
	const auto selected = read_bits(element, shape.lane_bytes, index);
	for (unsigned lane = 0;
	     lane < active_bytes(shape) / shape.lane_bytes; ++lane)
		write_bits(result, shape.lane_bytes, lane,
			exact_result_bits(operation, shape.lane_bytes,
				read_bits(accumulator, shape.lane_bytes, lane),
				read_bits(left, shape.lane_bytes, lane), selected));
	auto expected = initial;
	expected.vectors[registers.destination].bytes = result;
	return expected;
}

inline void prepare_exact_inputs(arm64::CpuState &initial,
	const Shape &shape, Registers registers, unsigned index)
{
	const auto poison = floating_bits(shape.lane_bytes, -1.0);
	for (auto &reg : initial.vectors)
		fill_bits(reg.bytes, shape.lane_bytes, poison);
	fill_bits(initial.vectors[registers.destination].bytes,
		shape.lane_bytes, floating_bits(shape.lane_bytes, 4.0));
	for (unsigned lane = 0; lane < 16U / shape.lane_bytes; ++lane)
		write_bits(initial.vectors[registers.left].bytes, shape.lane_bytes,
			lane, floating_bits(shape.lane_bytes,
				2.0 + static_cast<double>(lane & 1U)));
	fill_bits(initial.vectors[registers.element].bytes,
		shape.lane_bytes, floating_bits(shape.lane_bytes, -3.0));
	write_bits(initial.vectors[registers.element].bytes, shape.lane_bytes,
		index, floating_bits(shape.lane_bytes, 3.0));
	initial.fpcr = 0U;
	initial.fpsr = fpsr_saturation;
}

inline void check_exact(engine_tests::EngineHarness &harness,
	std::string_view name, Operation operation, const Shape &shape,
	Registers registers, unsigned index, const arm64::CpuState &initial)
{
	harness.check_one(name, instruction(operation, shape,
		registers.destination, registers.left, registers.element, index),
		initial, expected_exact(initial, operation, shape, registers, index),
		native_requirements(shape, initial.fpcr));
}

} // namespace arm64_simd_fp_indexed_test
