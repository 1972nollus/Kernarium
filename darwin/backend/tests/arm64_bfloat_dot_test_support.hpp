// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_bfloat_arithmetic_test_support.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace arm64_bfloat_dot_test {

namespace shared = arm64_bfloat_arithmetic_test;
namespace arm64 = shared::arm64;
namespace engine_tests = shared::engine_tests;

struct Form final {
	bool by_element;
	bool wide;
	unsigned index;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array forms{
	Form{false, false, 0U}, Form{false, true, 0U},
	Form{true, false, 0U}, Form{true, false, 1U},
	Form{true, false, 2U}, Form{true, false, 3U},
	Form{true, true, 0U}, Form{true, true, 1U},
	Form{true, true, 2U}, Form{true, true, 3U},
};

inline constexpr std::array alias_cases{
	Registers{1U, 1U, 2U}, Registers{2U, 1U, 2U},
	Registers{1U, 2U, 2U}, Registers{3U, 3U, 3U},
	Registers{31U, 30U, 29U},
};

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	Registers registers = {0U, 1U, 2U}) noexcept
{
	const std::uint32_t base =
		form.by_element ? 0x0f40'f000U : 0x2e40'fc00U;
	const std::uint32_t index = form.by_element ?
		((form.index & 1U) << 21U) |
			((form.index & 2U) << 10U) : 0U;
	return base |
		(static_cast<std::uint32_t>(form.wide) << 30U) |
		index | ((registers.second & 31U) << 16U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t raw_vector_instruction(
	bool q, bool u, unsigned size, unsigned opcode,
	Registers registers = {0U, 1U, 2U}) noexcept
{
	return 0x0e00'8400U |
		(static_cast<std::uint32_t>(q) << 30U) |
		(static_cast<std::uint32_t>(u) << 29U) |
		((size & 3U) << 22U) | ((opcode & 15U) << 11U) |
		((registers.second & 31U) << 16U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

[[nodiscard]] inline std::uint32_t fixed_product(
	std::uint16_t left_bits, std::uint16_t right_bits,
	std::uint32_t fpcr)
{
	bool ignored{};
	const auto left = shared::unpack_bfloat(left_bits, true, ignored);
	const auto right = shared::unpack_bfloat(right_bits, true, ignored);
	if (left.type == shared::FloatingClass::nan ||
	    right.type == shared::FloatingClass::nan)
		return shared::default_nan(fpcr);
	const bool left_zero =
		left.type == shared::FloatingClass::finite &&
		left.magnitude.zero();
	const bool right_zero =
		right.type == shared::FloatingClass::finite &&
		right.magnitude.zero();
	if ((left.type == shared::FloatingClass::infinity && right_zero) ||
	    (right.type == shared::FloatingClass::infinity && left_zero))
		return shared::default_nan(fpcr);
	const bool negative = left.negative != right.negative;
	if (left.type == shared::FloatingClass::infinity ||
	    right.type == shared::FloatingClass::infinity)
		return (static_cast<std::uint32_t>(negative) << 31U) |
			0x7f80'0000U;
	if (left_zero || right_zero)
		return static_cast<std::uint32_t>(negative) << 31U;
	return shared::round_single(shared::multiply_exact(left, right),
		4U, true, true, false).bits;
}

[[nodiscard]] inline std::uint32_t fixed_add(
	std::uint32_t left_bits, std::uint32_t right_bits,
	std::uint32_t fpcr)
{
	bool ignored{};
	const auto left = shared::unpack_single(left_bits, true, ignored);
	const auto right = shared::unpack_single(right_bits, true, ignored);
	if (left.type == shared::FloatingClass::nan ||
	    right.type == shared::FloatingClass::nan)
		return shared::default_nan(fpcr);
	if (left.type == shared::FloatingClass::infinity &&
	    right.type == shared::FloatingClass::infinity &&
	    left.negative != right.negative)
		return shared::default_nan(fpcr);
	if (left.type == shared::FloatingClass::infinity)
		return left.raw;
	if (right.type == shared::FloatingClass::infinity)
		return right.raw;
	const auto sum = shared::add_exact(shared::exact(left),
		shared::exact(right), false);
	return shared::round_single(sum, 4U, true, true, false).bits;
}

[[nodiscard]] inline std::uint32_t extended_product_sum(
	std::uint16_t left_a_bits, std::uint16_t left_b_bits,
	std::uint16_t right_a_bits, std::uint16_t right_b_bits,
	std::uint32_t fpcr)
{
	const bool flush_input =
		(fpcr & shared::fpcr_fiz) != 0U ||
		((fpcr & shared::fpcr_fz) != 0U &&
		 (fpcr & shared::fpcr_ah) == 0U);
	bool ignored{};
	const auto left_a =
		shared::unpack_bfloat(left_a_bits, flush_input, ignored);
	const auto left_b =
		shared::unpack_bfloat(left_b_bits, flush_input, ignored);
	const auto right_a =
		shared::unpack_bfloat(right_a_bits, flush_input, ignored);
	const auto right_b =
		shared::unpack_bfloat(right_b_bits, flush_input, ignored);
	if (left_a.type == shared::FloatingClass::nan ||
	    left_b.type == shared::FloatingClass::nan ||
	    right_a.type == shared::FloatingClass::nan ||
	    right_b.type == shared::FloatingClass::nan)
		return shared::default_nan(fpcr);
	const bool zero_a =
		(left_a.type == shared::FloatingClass::finite &&
		 left_a.magnitude.zero()) ||
		(right_a.type == shared::FloatingClass::finite &&
		 right_a.magnitude.zero());
	const bool zero_b =
		(left_b.type == shared::FloatingClass::finite &&
		 left_b.magnitude.zero()) ||
		(right_b.type == shared::FloatingClass::finite &&
		 right_b.magnitude.zero());
	const bool infinity_a =
		left_a.type == shared::FloatingClass::infinity ||
		right_a.type == shared::FloatingClass::infinity;
	const bool infinity_b =
		left_b.type == shared::FloatingClass::infinity ||
		right_b.type == shared::FloatingClass::infinity;
	const bool sign_a = left_a.negative != right_a.negative;
	const bool sign_b = left_b.negative != right_b.negative;
	if ((zero_a && infinity_a) || (zero_b && infinity_b) ||
	    (infinity_a && infinity_b && sign_a != sign_b))
		return shared::default_nan(fpcr);
	if (infinity_a || infinity_b) {
		const bool negative = infinity_a ? sign_a : sign_b;
		return (static_cast<std::uint32_t>(negative) << 31U) |
			0x7f80'0000U;
	}
	const auto product_a = zero_a ?
		shared::ExactValue{sign_a, 0, 0} :
		shared::multiply_exact(left_a, right_a);
	const auto product_b = zero_b ?
		shared::ExactValue{sign_b, 0, 0} :
		shared::multiply_exact(left_b, right_b);
	const unsigned rounding = (fpcr >> 22U) & 3U;
	const auto sum = shared::add_exact(product_a, product_b,
		rounding == 2U);
	return shared::round_single(sum, rounding,
		(fpcr & shared::fpcr_fz) != 0U, false, false).bits;
}

[[nodiscard]] inline std::uint32_t extended_add(
	std::uint32_t left_bits, std::uint32_t right_bits,
	std::uint32_t fpcr)
{
	const bool flush_input =
		(fpcr & shared::fpcr_fiz) != 0U ||
		((fpcr & shared::fpcr_fz) != 0U &&
		 (fpcr & shared::fpcr_ah) == 0U);
	bool ignored{};
	const auto left =
		shared::unpack_single(left_bits, flush_input, ignored);
	const auto right =
		shared::unpack_single(right_bits, flush_input, ignored);
	if (left.type == shared::FloatingClass::nan ||
	    right.type == shared::FloatingClass::nan)
		return shared::default_nan(fpcr);
	if (left.type == shared::FloatingClass::infinity &&
	    right.type == shared::FloatingClass::infinity &&
	    left.negative != right.negative)
		return shared::default_nan(fpcr);
	if (left.type == shared::FloatingClass::infinity)
		return left.raw;
	if (right.type == shared::FloatingClass::infinity)
		return right.raw;
	const unsigned rounding = (fpcr >> 22U) & 3U;
	const auto sum = shared::add_exact(shared::exact(left),
		shared::exact(right), rounding == 2U);
	return shared::round_single(sum, rounding,
		(fpcr & shared::fpcr_fz) != 0U, false, false).bits;
}

[[nodiscard]] inline std::uint32_t dot_add(std::uint32_t addend,
	std::uint16_t left_a, std::uint16_t left_b,
	std::uint16_t right_a, std::uint16_t right_b,
	std::uint32_t fpcr)
{
	if ((fpcr & shared::fpcr_ebf) == 0U) {
		const auto product_a = fixed_product(left_a, right_a, fpcr);
		const auto product_b = fixed_product(left_b, right_b, fpcr);
		return fixed_add(addend,
			fixed_add(product_a, product_b, fpcr), fpcr);
	}
	const auto product_sum = extended_product_sum(left_a, left_b,
		right_a, right_b, fpcr | shared::fpcr_dn);
	return extended_add(addend, product_sum,
		fpcr | shared::fpcr_dn);
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, Form form, Registers registers)
{
	const auto first = initial.vectors[registers.first].bytes;
	const auto second = initial.vectors[registers.second].bytes;
	const auto addends = initial.vectors[registers.destination].bytes;
	auto expected = initial;
	std::array<std::byte, 16> result{};
	const unsigned lanes = form.wide ? 4U : 2U;
	for (unsigned lane = 0U; lane < lanes; ++lane) {
		const unsigned first_pair = lane * 2U;
		const unsigned second_pair =
			form.by_element ? form.index * 2U : first_pair;
		shared::write_single(result, lane,
			dot_add(shared::read_single(addends, lane),
				shared::read_bfloat(first, first_pair),
				shared::read_bfloat(first, first_pair + 1U),
				shared::read_bfloat(second, second_pair),
				shared::read_bfloat(second, second_pair + 1U),
				initial.fpcr));
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
	std::string_view name, Form form, Registers registers,
	const arm64::CpuState &initial, shared::ExpectedCounts &counts)
{
	const auto native = native_requirements(initial);
	harness.check_one(name, instruction(form, registers), initial,
		expected_state(initial, form, registers), native);
	counts.add(native);
}

} // namespace arm64_bfloat_dot_test
