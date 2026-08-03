// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_bfloat_arithmetic_test_support.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace arm64_bfloat_multiply_test {

namespace shared = arm64_bfloat_arithmetic_test;
namespace arm64 = shared::arm64;
namespace engine_tests = shared::engine_tests;

enum class Form : unsigned {
	vector_bottom,
	vector_top,
	indexed_bottom,
	indexed_top,
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

struct MultiplyAddResult final {
	std::uint32_t bits;
	std::uint32_t flags;
};

inline constexpr std::array forms{
	Form::vector_bottom, Form::vector_top,
	Form::indexed_bottom, Form::indexed_top,
};

inline constexpr std::array alias_cases{
	Registers{1U, 1U, 2U}, Registers{2U, 1U, 2U},
	Registers{1U, 2U, 2U}, Registers{3U, 3U, 3U},
	Registers{31U, 30U, 15U},
};

[[nodiscard]] constexpr bool top(Form form) noexcept
{
	return form == Form::vector_top || form == Form::indexed_top;
}

[[nodiscard]] constexpr bool by_element(Form form) noexcept
{
	return form == Form::indexed_bottom ||
		form == Form::indexed_top;
}

[[nodiscard]] constexpr unsigned index_count(Form form) noexcept
{
	return by_element(form) ? 8U : 1U;
}

[[nodiscard]] constexpr unsigned second_register_count(Form form) noexcept
{
	return by_element(form) ? 16U : 32U;
}

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	unsigned index = 0U,
	Registers registers = {0U, 1U, 2U}) noexcept
{
	std::uint32_t encoding;
	if (by_element(form)) {
		encoding = 0x0fc0'f000U |
			((index & 3U) << 20U) |
			((index & 4U) << 9U) |
			((registers.second & 15U) << 16U);
	} else {
		encoding = 0x2ec0'fc00U |
			((registers.second & 31U) << 16U);
	}
	return encoding |
		(static_cast<std::uint32_t>(top(form)) << 30U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr bool is_single_subnormal(
	std::uint32_t bits) noexcept
{
	return (bits & 0x7f80'0000U) == 0U &&
		(bits & 0x007f'ffffU) != 0U;
}

[[nodiscard]] constexpr bool is_bfloat_subnormal(
	std::uint16_t bits) noexcept
{
	return (bits & 0x7f80U) == 0U &&
		(bits & 0x007fU) != 0U;
}

[[nodiscard]] constexpr bool is_nan(std::uint32_t bits) noexcept
{
	return (bits & 0x7f80'0000U) == 0x7f80'0000U &&
		(bits & 0x007f'ffffU) != 0U;
}

[[nodiscard]] constexpr bool is_signaling_nan(
	std::uint32_t bits) noexcept
{
	return is_nan(bits) && (bits & 0x0040'0000U) == 0U;
}

[[nodiscard]] constexpr bool is_infinity(std::uint32_t bits) noexcept
{
	return (bits & 0x7fff'ffffU) == 0x7f80'0000U;
}

[[nodiscard]] constexpr bool is_zero(std::uint32_t bits) noexcept
{
	return (bits & 0x7fff'ffffU) == 0U;
}

[[nodiscard]] constexpr std::uint32_t flush_subnormal(
	std::uint32_t bits) noexcept
{
	return is_single_subnormal(bits) ? bits & 0x8000'0000U : bits;
}

[[nodiscard]] inline MultiplyAddResult alternative_multiply_add(
	std::uint32_t addend, std::uint32_t left, std::uint32_t right,
	std::uint32_t fpcr)
{
	addend = flush_subnormal(addend);
	left = flush_subnormal(left);
	right = flush_subnormal(right);
	const bool addend_nan = is_nan(addend);
	const bool left_nan = is_nan(left);
	const bool right_nan = is_nan(right);
	std::uint32_t selected_nan{};
	if (left_nan && (addend_nan || right_nan))
		selected_nan = left;
	else if (right_nan && addend_nan)
		selected_nan = right;
	else if (is_signaling_nan(addend))
		selected_nan = addend;
	else if (is_signaling_nan(left))
		selected_nan = left;
	else if (is_signaling_nan(right))
		selected_nan = right;
	else if (addend_nan)
		selected_nan = addend;
	else if (left_nan)
		selected_nan = left;
	else if (right_nan)
		selected_nan = right;
	if (selected_nan != 0U) {
		const auto result = (fpcr & shared::fpcr_dn) != 0U ?
			0xffc0'0000U : selected_nan | 0x0040'0000U;
		return {result, 0U};
	}

	const bool left_infinity = is_infinity(left);
	const bool right_infinity = is_infinity(right);
	const bool left_zero = is_zero(left);
	const bool right_zero = is_zero(right);
	const bool addend_infinity = is_infinity(addend);
	const bool product_infinity = left_infinity || right_infinity;
	const bool product_zero = left_zero || right_zero;
	const bool product_negative = ((left ^ right) >> 31U) != 0U;
	const bool addend_negative = (addend >> 31U) != 0U;
	if ((left_infinity && right_zero) ||
	    (left_zero && right_infinity) ||
	    (addend_infinity && product_infinity &&
	     addend_negative != product_negative))
		return {0xffc0'0000U, 0U};
	if (addend_infinity)
		return {addend, 0U};
	if (product_infinity)
		return {
			(static_cast<std::uint32_t>(product_negative) << 31U) |
				0x7f80'0000U,
			0U};
	if (is_zero(addend) && product_zero &&
	    addend_negative == product_negative)
		return {
			static_cast<std::uint32_t>(addend_negative) << 31U,
			0U};

	bool ignored{};
	const auto unpacked_addend =
		shared::unpack_single(addend, false, ignored);
	const auto unpacked_left =
		shared::unpack_single(left, false, ignored);
	const auto unpacked_right =
		shared::unpack_single(right, false, ignored);
	const auto product =
		shared::multiply_exact(unpacked_left, unpacked_right);
	const auto sum = shared::add_exact(shared::exact(unpacked_addend),
		product, false);
	return {shared::round_single(sum, 0U, true, false, false).bits,
		0U};
}

[[nodiscard]] inline MultiplyAddResult standard_multiply_add(
	std::uint32_t addend, std::uint32_t left, std::uint32_t right,
	std::uint32_t fpcr)
{
	const bool flush_with_flag =
		(fpcr & shared::fpcr_fz) != 0U;
	const bool flush_input = flush_with_flag ||
		(fpcr & shared::fpcr_fiz) != 0U;
	std::uint32_t flags{};
	if (flush_with_flag &&
	    (is_single_subnormal(addend) ||
	     is_single_subnormal(left) ||
	     is_single_subnormal(right)))
		flags |= shared::fpsr_input_denormal;
	bool ignored{};
	const auto unpacked_addend =
		shared::unpack_single(addend, flush_input, ignored);
	const auto unpacked_left =
		shared::unpack_single(left, flush_input, ignored);
	const auto unpacked_right =
		shared::unpack_single(right, flush_input, ignored);

	if (unpacked_addend.type == shared::FloatingClass::nan ||
	    unpacked_left.type == shared::FloatingClass::nan ||
	    unpacked_right.type == shared::FloatingClass::nan) {
		const bool signaling = unpacked_addend.signaling ||
			unpacked_left.signaling || unpacked_right.signaling;
		if (signaling)
			flags |= shared::fpsr_invalid;
		if ((fpcr & shared::fpcr_dn) != 0U)
			return {0x7fc0'0000U, flags};
		const auto selected = unpacked_left.type ==
				shared::FloatingClass::nan ?
			unpacked_left.raw :
			unpacked_right.type == shared::FloatingClass::nan ?
				unpacked_right.raw : unpacked_addend.raw;
		return {selected | 0x0040'0000U, flags};
	}

	const bool product_zero =
		(unpacked_left.type == shared::FloatingClass::finite &&
		 unpacked_left.magnitude.zero()) ||
		(unpacked_right.type == shared::FloatingClass::finite &&
		 unpacked_right.magnitude.zero());
	const bool product_infinity =
		unpacked_left.type == shared::FloatingClass::infinity ||
		unpacked_right.type == shared::FloatingClass::infinity;
	const bool product_negative =
		unpacked_left.negative != unpacked_right.negative;
	if ((product_zero && product_infinity) ||
	    (unpacked_addend.type == shared::FloatingClass::infinity &&
	     product_infinity &&
	     unpacked_addend.negative != product_negative))
		return {0x7fc0'0000U, flags | shared::fpsr_invalid};
	if (unpacked_addend.type == shared::FloatingClass::infinity)
		return {unpacked_addend.raw, flags};
	if (product_infinity)
		return {
			(static_cast<std::uint32_t>(product_negative) << 31U) |
				0x7f80'0000U,
			flags};

	const auto product = product_zero ?
		shared::ExactValue{product_negative, 0, 0} :
		shared::multiply_exact(unpacked_left, unpacked_right);
	const unsigned rounding = (fpcr >> 22U) & 3U;
	const auto sum = shared::add_exact(shared::exact(unpacked_addend),
		product, rounding == 2U);
	const auto rounded = shared::round_single(sum, rounding,
		(fpcr & shared::fpcr_fz) != 0U, false, true);
	return {rounded.bits, flags | rounded.flags};
}

[[nodiscard]] inline MultiplyAddResult multiply_add(
	std::uint32_t addend, std::uint16_t left,
	std::uint16_t right, std::uint32_t fpcr)
{
	const auto left32 = static_cast<std::uint32_t>(left) << 16U;
	const auto right32 = static_cast<std::uint32_t>(right) << 16U;
	if ((fpcr & shared::fpcr_ah) != 0U)
		return alternative_multiply_add(addend, left32, right32,
			fpcr);
	return standard_multiply_add(addend, left32, right32, fpcr);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, Form form, unsigned index,
	Registers registers)
{
	const auto addends = initial.vectors[registers.destination].bytes;
	const auto first = initial.vectors[registers.first].bytes;
	const auto second = initial.vectors[registers.second].bytes;
	auto expected = initial;
	auto result = addends;
	std::uint32_t flags{};
	for (unsigned lane = 0U; lane < 4U; ++lane) {
		const unsigned first_lane =
			lane * 2U + static_cast<unsigned>(top(form));
		const unsigned second_lane = by_element(form) ?
			index : lane * 2U + static_cast<unsigned>(top(form));
		const auto lane_result = multiply_add(
			shared::read_single(addends, lane),
			shared::read_bfloat(first, first_lane),
			shared::read_bfloat(second, second_lane),
			initial.fpcr);
		shared::write_single(result, lane, lane_result.bits);
		flags |= lane_result.flags;
	}
	expected.vectors[registers.destination].bytes = result;
	expected.fpsr |= flags;
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements(
	const arm64::CpuState &initial)
{
	return shared::native_requirements(initial.fpcr, false);
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, Form form, unsigned index,
	Registers registers, const arm64::CpuState &initial,
	shared::ExpectedCounts &counts)
{
	const auto native = native_requirements(initial);
	harness.check_one(name, instruction(form, index, registers),
		initial, expected_state(initial, form, index, registers),
		native);
	counts.add(native);
}

} // namespace arm64_bfloat_multiply_test
