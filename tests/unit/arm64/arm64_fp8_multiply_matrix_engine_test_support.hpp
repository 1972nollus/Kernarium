// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_fp8_dot_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_fp8_multiply_matrix_engine_test {

namespace oracle = arm64_fp8_dot_engine_test;
namespace source_oracle = arm64_fp8_convert_up_engine_test;
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Family : std::uint8_t {
	multiply,
	matrix,
};

struct Form final {
	Family family;
	unsigned width;
	bool by_element;
	unsigned selection;
	unsigned index;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array register_aliases{
	Registers{0U, 1U, 2U}, Registers{1U, 1U, 2U},
	Registers{2U, 1U, 2U}, Registers{1U, 2U, 2U},
	Registers{3U, 3U, 3U}, Registers{31U, 30U, 29U},
};

[[nodiscard]] constexpr bool is_multiply(const Form &form) noexcept
{
	return form.family == Family::multiply;
}

[[nodiscard]] constexpr unsigned selection_count(
	const Form &form) noexcept
{
	return form.width == 16U ? 2U : 4U;
}

[[nodiscard]] constexpr unsigned index_count(const Form &) noexcept
{
	return 16U;
}

[[nodiscard]] constexpr std::uint32_t instruction(
	const Form &form, Registers registers = {0U, 1U, 2U}) noexcept
{
	if (form.family == Family::matrix)
		return (form.width == 16U ? 0x6e00'ec00U : 0x6e80'ec00U) |
			((registers.second & 31U) << 16U) |
			((registers.first & 31U) << 5U) |
			(registers.destination & 31U);

	std::uint32_t result;
	if (form.width == 16U)
		result = (form.by_element ? 0x0fc0'0000U :
			0x0ec0'fc00U) |
			((form.selection & 1U) << 30U);
	else
		result = (form.by_element ? 0x2f00'8000U :
			0x0e00'c400U) |
			((form.selection >> 1U) << 30U) |
			((form.selection & 1U) << 22U);
	result |= ((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
	if (!form.by_element)
		return result | ((registers.second & 31U) << 16U);
	result |= (registers.second & 7U) << 16U;
	result |= (form.index & 1U) << 19U;
	result |= ((form.index >> 1U) & 1U) << 20U;
	result |= ((form.index >> 2U) & 1U) << 21U;
	result |= ((form.index >> 3U) & 1U) << 11U;
	return result;
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(const Form &form, std::uint32_t fpcr) noexcept
{
	using ::uml::cpu::ArmFeature;
	auto result = engine_tests::NativeRequirements::features(
		form.family == Family::multiply ? ArmFeature::fp8fma :
		form.width == 16U ? ArmFeature::f8f16mm :
			ArmFeature::f8f32mm);
	if ((fpcr & source_oracle::fpcr_ah) != 0U)
		result = result | ArmFeature::afp;
	return result;
}

[[nodiscard]] constexpr std::uint64_t fpmr(
	unsigned format1 = 0U, unsigned format2 = 0U,
	unsigned scale = 0U, bool saturate = false) noexcept
{
	return oracle::fpmr(format1, format2, scale, saturate);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
{
	return oracle::state(seed);
}

[[nodiscard]] inline std::uint8_t read_byte(
	const std::array<std::byte, 16> &bytes, unsigned index) noexcept
{
	return std::to_integer<std::uint8_t>(bytes[index]);
}

[[nodiscard]] inline std::uint32_t multiply_lane(
	std::uint32_t addend, unsigned width, std::uint8_t first,
	std::uint8_t second, std::uint32_t fpcr, std::uint64_t control)
{
	const std::array left{first};
	const std::array right{second};
	return oracle::dot_add(addend,
		oracle::Form{width, false, false, 0U},
		std::span{left}, std::span{right}, fpcr, control);
}

template<std::size_t Count>
[[nodiscard]] inline std::uint32_t matrix_lane(
	std::uint32_t addend, unsigned width,
	const std::array<std::byte, 16> &first, unsigned first_offset,
	const std::array<std::byte, 16> &second, unsigned second_offset,
	std::uint32_t fpcr, std::uint64_t control)
{
	std::array<std::uint8_t, Count> left{};
	std::array<std::uint8_t, Count> right{};
	for (unsigned index = 0U; index < Count; ++index) {
		left[index] = read_byte(first, first_offset + index);
		right[index] = read_byte(second, second_offset + index);
	}
	return oracle::dot_add(addend,
		oracle::Form{width, false, false, 0U},
		std::span{left}, std::span{right}, fpcr, control);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, const Form &form,
	Registers registers)
{
	const auto destination =
		initial.vectors[registers.destination].bytes;
	const auto first = initial.vectors[registers.first].bytes;
	const auto second = initial.vectors[registers.second].bytes;
	auto expected = initial;
	auto result = destination;

	if (form.family == Family::multiply) {
		const unsigned lanes = 128U / form.width;
		const unsigned stride = form.width / 8U;
		for (unsigned lane = 0U; lane < lanes; ++lane) {
			const unsigned left =
				lane * stride + form.selection;
			const unsigned right = form.by_element ?
				form.index : left;
			oracle::write_lane(result, lane, form.width,
				multiply_lane(
					oracle::read_lane(destination, lane,
						form.width),
					form.width, read_byte(first, left),
					read_byte(second, right),
					initial.fpcr, initial.fpmr));
		}
	} else if (form.width == 16U) {
		for (unsigned segment = 0U; segment < 2U; ++segment)
			for (unsigned row = 0U; row < 2U; ++row)
				for (unsigned column = 0U; column < 2U;
				     ++column) {
					const unsigned lane =
						segment * 4U +
						row * 2U + column;
					oracle::write_lane(result, lane, 16U,
						matrix_lane<4>(
							oracle::read_lane(
								destination,
								lane, 16U),
							16U, first,
							segment * 8U +
								row * 4U,
							second,
							segment * 8U +
								column * 4U,
							initial.fpcr,
							initial.fpmr));
				}
	} else {
		for (unsigned row = 0U; row < 2U; ++row)
			for (unsigned column = 0U; column < 2U;
			     ++column) {
				const unsigned lane = row * 2U + column;
				oracle::write_lane(result, lane, 32U,
					matrix_lane<8>(
						oracle::read_lane(destination,
							lane, 32U),
						32U, first, row * 8U,
						second, column * 8U,
						initial.fpcr, initial.fpmr));
			}
	}

	expected.vectors[registers.destination].bytes = result;
	return expected;
}

inline void check(
	engine_tests::EngineHarness &harness, std::string_view name,
	const Form &form, Registers registers,
	const arm64::CpuState &initial)
{
	harness.check_one(name, instruction(form, registers), initial,
		expected_state(initial, form, registers),
		native_requirements(form, initial.fpcr));
}

template<typename Function>
void for_each_multiply_form(Function &&function)
{
	for (const unsigned width : {16U, 32U}) {
		const Form prototype{
			Family::multiply, width, false, 0U, 0U};
		for (unsigned selection = 0U;
		     selection < selection_count(prototype); ++selection) {
			function(Form{Family::multiply, width, false,
				selection, 0U});
			for (unsigned index = 0U;
			     index < index_count(prototype); ++index)
				function(Form{Family::multiply, width, true,
					selection, index});
		}
	}
}

template<typename Function>
void for_each_matrix_form(Function &&function)
{
	for (const unsigned width : {16U, 32U})
		function(Form{Family::matrix, width, false, 0U, 0U});
}

inline constexpr auto representative_inputs =
	oracle::representative_inputs;
inline constexpr auto half_addends = oracle::half_addends;
inline constexpr auto single_addends = oracle::single_addends;

} // namespace arm64_fp8_multiply_matrix_engine_test
