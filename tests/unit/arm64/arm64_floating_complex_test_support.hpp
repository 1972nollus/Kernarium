// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_basic_floating_test_support.hpp"
#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <type_traits>

namespace arm64_floating_complex_test {

namespace basic = arm64_basic_floating_test;
namespace common = arm64_simd_test;
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	add,
	multiply_accumulate,
};

struct Form final {
	Operation operation;
	bool by_element;
	unsigned width;
	unsigned vector_bytes;
	unsigned rotation;
	unsigned index;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array register_cases{
	Registers{0, 1, 2},
	Registers{1, 1, 2},
	Registers{2, 1, 2},
	Registers{1, 2, 1},
	Registers{1, 1, 1},
	Registers{31, 30, 29},
	Registers{30, 31, 30},
};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto shape : basic::vector_shapes)
		for (const unsigned rotation : {1U, 3U})
			function(Form{Operation::add, false, shape[0], shape[1],
				rotation, 0U});
	for (const auto shape : basic::vector_shapes)
		for (unsigned rotation = 0; rotation < 4U; ++rotation)
			function(Form{Operation::multiply_accumulate, false,
				shape[0], shape[1], rotation, 0U});
	for (unsigned index = 0; index < 2U; ++index)
		for (unsigned rotation = 0; rotation < 4U; ++rotation)
			function(Form{Operation::multiply_accumulate, true,
				16U, 8U, rotation, index});
	for (unsigned index = 0; index < 4U; ++index)
		for (unsigned rotation = 0; rotation < 4U; ++rotation)
			function(Form{Operation::multiply_accumulate, true,
				16U, 16U, rotation, index});
	for (unsigned index = 0; index < 2U; ++index)
		for (unsigned rotation = 0; rotation < 4U; ++rotation)
			function(Form{Operation::multiply_accumulate, true,
				32U, 16U, rotation, index});
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
	constexpr std::array add{
		0x2e40'e400U, 0x6e40'e400U, 0x2e80'e400U,
		0x6e80'e400U, 0x6ec0'e400U,
	};
	constexpr std::array multiply{
		0x2e40'c400U, 0x6e40'c400U, 0x2e80'c400U,
		0x6e80'c400U, 0x6ec0'c400U,
	};
	std::uint32_t value;
	if (form.operation == Operation::add) {
		value = add[vector_shape(form.width, form.vector_bytes)];
		if (form.rotation == 3U)
			value |= 0x1000U;
	} else if (!form.by_element) {
		value = multiply[vector_shape(form.width, form.vector_bytes)] |
			(form.rotation << 11);
	} else {
		value = form.width == 32U ? 0x6f80'1000U :
			form.vector_bytes == 16U ? 0x6f40'1000U :
			0x2f40'1000U;
		if (form.width == 16U) {
			value |= (form.index & 1U) << 21;
			value |= (form.index >> 1) << 11;
		} else {
			value |= form.index << 11;
		}
		value |= form.rotation << 13;
	}
	value |= registers.destination & 31U;
	value |= (registers.first & 31U) << 5;
	if (form.by_element) {
		value |= (registers.second & 15U) << 16;
		value |= ((registers.second >> 4) & 1U) << 20;
	} else {
		value |= (registers.second & 31U) << 16;
	}
	return value;
}

[[nodiscard]] constexpr std::uint32_t raw_vector_instruction(
	Operation operation, bool q, unsigned size, unsigned rotation,
	Registers registers = {0, 1, 2}) noexcept
{
	auto value = operation == Operation::add ?
		0x2e00'e400U : 0x2e00'c400U;
	value |= q ? 0x4000'0000U : 0U;
	value |= (size & 3U) << 22;
	if (operation == Operation::add)
		value |= (rotation & 1U) << 12;
	else
		value |= (rotation & 3U) << 11;
	value |= registers.destination & 31U;
	value |= (registers.first & 31U) << 5;
	value |= (registers.second & 31U) << 16;
	return value;
}

[[nodiscard]] constexpr std::uint32_t raw_element_instruction(
	bool q, unsigned size, bool l, bool h, unsigned rotation,
	Registers registers = {0, 1, 2}) noexcept
{
	auto value = 0x2f00'1000U;
	value |= q ? 0x4000'0000U : 0U;
	value |= (size & 3U) << 22;
	value |= l ? 0x0020'0000U : 0U;
	value |= h ? 0x0000'0800U : 0U;
	value |= (rotation & 3U) << 13;
	value |= registers.destination & 31U;
	value |= (registers.first & 31U) << 5;
	value |= (registers.second & 15U) << 16;
	value |= ((registers.second >> 4) & 1U) << 20;
	return value;
}

[[nodiscard]] constexpr bool valid_vector_control(bool q,
	unsigned size) noexcept
{
	return size != 0U && (size != 3U || q);
}

[[nodiscard]] constexpr bool valid_element_control(bool q, unsigned size,
	bool l, bool h) noexcept
{
	if (size == 1U)
		return q || !h;
	if (size == 2U)
		return q && !l;
	return false;
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(const Form &form)
{
	using ::uml::cpu::ArmFeature;
	auto requirements = engine_tests::NativeRequirements::features({
		ArmFeature::advanced_simd,
		ArmFeature::fcma,
	});
	return form.width == 16U ?
		requirements | ArmFeature::fp16 : requirements;
}

struct ExpectedCounts final {
	std::size_t cases{};
	std::size_t jit{};
	std::size_t feature_skips{};

	void add(engine_tests::NativeRequirements requirements,
		bool invalid_encoding = false)
	{
		++cases;
		if (invalid_encoding || engine_tests::host_supports(requirements))
			++jit;
		else
			++feature_skips;
	}
};

inline void require_counts(const engine_tests::EngineHarness &harness,
	const ExpectedCounts &expected)
{
	const auto &actual = harness.counts();
	common::require(actual.interpreter == expected.cases);
	common::require(actual.gadgets == expected.cases);
	common::require(actual.jit == expected.jit);
	common::require(actual.jit_skipped_host_feature ==
		expected.feature_skips);
	common::require(actual.jit_skipped == expected.feature_skips);
	common::require(actual.jit_unavailable == 0U);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	return basic::state(seed);
}

[[nodiscard]] constexpr unsigned active_lanes(const Form &form) noexcept
{
	return form.vector_bytes / (form.width / 8U);
}

inline void set_lane_bits(arm64::CpuState &cpu, unsigned reg,
	unsigned lane, unsigned width, std::uint64_t bits)
{
	basic::set_lane_bits(cpu, reg, lane, width, bits);
}

inline void fill_active_lanes(arm64::CpuState &cpu, unsigned reg,
	const Form &form, std::uint64_t bits)
{
	for (unsigned lane = 0; lane < active_lanes(form); ++lane)
		set_lane_bits(cpu, reg, lane, form.width, bits);
}

template<typename Floating>
using FloatingInteger = std::conditional_t<sizeof(Floating) == 2,
	std::uint16_t, std::conditional_t<sizeof(Floating) == 4,
	std::uint32_t, std::uint64_t>>;

template<typename Floating>
[[nodiscard]] constexpr FloatingInteger<Floating> bits(Floating value) noexcept
{
	return std::bit_cast<FloatingInteger<Floating>>(value);
}

template<typename Floating>
void set_pair(arm64::CpuState &cpu, unsigned reg, unsigned pair,
	Floating real, Floating imaginary)
{
	basic::set_lane(cpu, reg, pair * 2U, real);
	basic::set_lane(cpu, reg, pair * 2U + 1U, imaginary);
}

[[nodiscard]] inline arm64::CpuState expected_lanes(
	const arm64::CpuState &initial, const Form &form, Registers registers,
	std::span<const std::uint64_t> lane_bits,
	std::uint32_t new_fpsr_flags = 0)
{
	auto expected = initial;
	expected.vectors[registers.destination].bytes.fill(std::byte{});
	for (unsigned lane = 0; lane < active_lanes(form); ++lane)
		set_lane_bits(expected, registers.destination, lane,
			form.width, lane_bits[lane]);
	expected.fpsr |= new_fpsr_flags;
	return expected;
}

[[nodiscard]] inline arm64::CpuState expected_uniform(
	const arm64::CpuState &initial, const Form &form, Registers registers,
	std::uint64_t result_bits, std::uint32_t new_fpsr_flags = 0)
{
	std::array<std::uint64_t, 8> results{};
	results.fill(result_bits);
	return expected_lanes(initial, form, registers,
		std::span<const std::uint64_t>{results}.first(active_lanes(form)),
		new_fpsr_flags);
}

inline void prepare_unit(arm64::CpuState &cpu, const Form &form,
	Registers registers)
{
	const auto one = basic::format_bits(form.width).one;
	for (const unsigned reg : {
		registers.destination, registers.first, registers.second,
	})
		fill_active_lanes(cpu, reg, form, one);
	if (form.by_element)
		for (unsigned lane = active_lanes(form);
		     lane < 128U / form.width; ++lane)
			set_lane_bits(cpu, registers.second, lane, form.width, one);
}

[[nodiscard]] inline arm64::CpuState expected_unit(
	const arm64::CpuState &initial, const Form &form, Registers registers)
{
	const auto &format = basic::format_bits(form.width);
	std::array<std::uint64_t, 8> results{};
	for (unsigned pair = 0; pair < active_lanes(form) / 2U; ++pair) {
		auto real = format.positive_zero;
		auto imaginary = format.positive_zero;
		if (form.operation == Operation::add) {
			real = form.rotation == 1U ?
				format.positive_zero : format.two;
			imaginary = form.rotation == 1U ?
				format.two : format.positive_zero;
		} else {
			switch (form.rotation) {
			case 0:
				real = format.two;
				imaginary = format.two;
				break;
			case 1:
				real = format.positive_zero;
				imaginary = format.two;
				break;
			case 2:
				real = format.positive_zero;
				imaginary = format.positive_zero;
				break;
			default:
				real = format.two;
				imaginary = format.positive_zero;
				break;
			}
		}
		results[pair * 2U] = real;
		results[pair * 2U + 1U] = imaginary;
	}
	return expected_lanes(initial, form, registers,
		std::span<const std::uint64_t>{results}.first(active_lanes(form)));
}

template<typename Floating>
void prepare_exact(arm64::CpuState &cpu, const Form &form,
	Registers registers)
{
	const unsigned pairs = active_lanes(form) / 2U;
	for (unsigned pair = 0; pair < pairs; ++pair) {
		if (form.operation == Operation::add) {
			set_pair(cpu, registers.first, pair,
				static_cast<Floating>(1),
				static_cast<Floating>(2));
		} else {
			set_pair(cpu, registers.destination, pair,
				static_cast<Floating>(10),
				static_cast<Floating>(20));
			set_pair(cpu, registers.first, pair,
				static_cast<Floating>(2),
				static_cast<Floating>(5));
		}
	}
	const unsigned source_pairs = 128U / form.width / 2U;
	for (unsigned pair = 0; pair < source_pairs; ++pair)
		set_pair(cpu, registers.second, pair,
			static_cast<Floating>(3 + pair * 2U),
			static_cast<Floating>(4 + pair * 2U));
}

template<typename Floating>
[[nodiscard]] arm64::CpuState expected_exact(
	const arm64::CpuState &initial, const Form &form, Registers registers)
{
	std::array<std::uint64_t, 8> results{};
	for (unsigned pair = 0; pair < active_lanes(form) / 2U; ++pair) {
		const unsigned source_pair =
			form.by_element ? form.index : pair;
		const Floating second_real =
			static_cast<Floating>(3 + source_pair * 2U);
		const Floating second_imaginary =
			static_cast<Floating>(4 + source_pair * 2U);
		Floating real;
		Floating imaginary;
		if (form.operation == Operation::add) {
			real = form.rotation == 1U ?
				static_cast<Floating>(1 - second_imaginary) :
				static_cast<Floating>(1 + second_imaginary);
			imaginary = form.rotation == 1U ?
				static_cast<Floating>(2 + second_real) :
				static_cast<Floating>(2 - second_real);
		} else {
			switch (form.rotation) {
			case 0:
				real = static_cast<Floating>(
					10 + 2 * second_real);
				imaginary = static_cast<Floating>(
					20 + 2 * second_imaginary);
				break;
			case 1:
				real = static_cast<Floating>(
					10 - 5 * second_imaginary);
				imaginary = static_cast<Floating>(
					20 + 5 * second_real);
				break;
			case 2:
				real = static_cast<Floating>(
					10 - 2 * second_real);
				imaginary = static_cast<Floating>(
					20 - 2 * second_imaginary);
				break;
			default:
				real = static_cast<Floating>(
					10 + 5 * second_imaginary);
				imaginary = static_cast<Floating>(
					20 - 5 * second_real);
				break;
			}
		}
		results[pair * 2U] = bits(real);
		results[pair * 2U + 1U] = bits(imaginary);
	}
	return expected_lanes(initial, form, registers,
		std::span<const std::uint64_t>{results}.first(active_lanes(form)));
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(
	unsigned reg) noexcept
{
	return offsetof(arm64::CpuState, vectors) +
		reg * sizeof(arm64::VectorRegister);
}

[[nodiscard]] constexpr std::uintptr_t gadget_operand(
	Registers registers) noexcept
{
	static_assert(vector_offset(31U) <= 0xffffU);
	return vector_offset(registers.destination) |
		(vector_offset(registers.first) << 16U) |
		(vector_offset(registers.second) << 32U);
}

inline void check(engine_tests::EngineHarness &harness,
	ExpectedCounts &counts, std::string_view name, const Form &form,
	Registers registers, const arm64::CpuState &initial,
	const arm64::CpuState &expected)
{
	const auto requirements = native_requirements(form);
	harness.check_one(name, instruction(form, registers), initial,
		expected, requirements);
	counts.add(requirements);
}

} // namespace arm64_floating_complex_test
