// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "floating_bits.hpp"
#include "floating_exception.hpp"
#include "half_fused_long.hpp"
#include "instruction_primitives.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::cpu::arm64::detail {

struct HalfMatrix final {
	bool recognized{};
	bool valid{};
	bool widening{};
};

[[nodiscard]] constexpr HalfMatrix decode_half_matrix(
	std::uint32_t instruction) noexcept
{
	const auto operation = instruction & 0xbfe0'fc00U;
	const bool q = (instruction & 0x4000'0000U) != 0U;
	if (operation == 0x0ec0'ec00U)
		return {true, q, false};
	if (operation == 0x2ec0'ec00U)
		return {true, false, false};
	if (operation == 0x0e40'ec00U)
		return {true, q, true};
	return {};
}

struct HalfMatrixResult final {
	std::array<std::byte, 16> bytes{};
	std::uint32_t exceptions{};
};

namespace half_matrix_detail {

using namespace floating_bits;

[[nodiscard]] constexpr std::uint32_t convert_half_nan(std::uint16_t value,
	std::uint32_t fpcr) noexcept
{
	if ((fpcr & (1U << 25)) != 0U)
		return default_nan<std::uint32_t>(fpcr);
	return (static_cast<std::uint32_t>(value & 0x8000U) << 16) |
		0x7fc0'0000U |
		(static_cast<std::uint32_t>(value & 0x01ffU) << 13);
}

struct BinaryResult final {
	std::uint64_t bits{};
	std::uint32_t exceptions{};
};

[[nodiscard]] inline BinaryResult captured_half_binary(std::uint16_t left,
	std::uint16_t right, std::uint32_t fpcr, bool multiply) noexcept
{
	const auto left_value = std::bit_cast<_Float16>(left);
	const auto right_value = std::bit_cast<_Float16>(right);
	const auto captured = run_with_local_fpcr(host_fpcr(fpcr), [&] {
		_Float16 result;
		if (multiply)
			__asm__ volatile("fmul %h0, %h1, %h2" : "=w"(result) :
				"w"(left_value), "w"(right_value));
		else
			__asm__ volatile("fadd %h0, %h1, %h2" : "=w"(result) :
				"w"(left_value), "w"(right_value));
		return result;
	});
	return {std::bit_cast<std::uint16_t>(captured.value), captured.fpsr};
}

[[nodiscard]] inline BinaryResult half_binary(std::uint16_t left,
	std::uint16_t right, std::uint32_t fpcr, bool multiply) noexcept
{
	const bool alternative = (fpcr & 2U) != 0U;
	if (!alternative)
		return captured_half_binary(left, right, fpcr, multiply);
	if ((fpcr & (1U << 19)) != 0U) {
		if (is_subnormal<std::uint16_t>(left))
			left &= 0x8000U;
		if (is_subnormal<std::uint16_t>(right))
			right &= 0x8000U;
	}

	const bool left_nan = is_nan<std::uint16_t>(left);
	const bool right_nan = is_nan<std::uint16_t>(right);
	if (left_nan || right_nan) {
		const bool any_signaling = is_signaling_nan<std::uint16_t>(left) ||
			is_signaling_nan<std::uint16_t>(right);
		const std::uint16_t selected = left_nan ? left : right;
		return {process_nan(selected, fpcr), any_signaling ? 1U : 0U};
	}
	const bool invalid = multiply ?
		((is_infinity<std::uint16_t>(left) && is_zero<std::uint16_t>(right)) ||
		 (is_zero<std::uint16_t>(left) && is_infinity<std::uint16_t>(right))) :
		(is_infinity<std::uint16_t>(left) && is_infinity<std::uint16_t>(right) &&
		 ((left ^ right) & 0x8000U) != 0U);
	if (invalid)
		return {default_nan<std::uint16_t>(fpcr), 1U};

	/* AH changes NaN priority and the timing of half underflow.  Run the
	 * ordinary finite operation with AH disabled, then apply the post-round
	 * FZ16 rule required by alternative floating-point behavior. */
	const bool flush_output = (fpcr & (1U << 19)) != 0U;
	auto result = captured_half_binary(left, right,
		fpcr & ~(2U | (1U << 19)), multiply);
	const auto bits = static_cast<std::uint16_t>(result.bits);
	if (is_subnormal<std::uint16_t>(bits)) {
		if (flush_output) {
			result.bits = bits & 0x8000U;
			result.exceptions |= (1U << 3) | (1U << 4);
		}
	} else {
		result.exceptions &= ~(1U << 3);
	}
	return result;
}

[[nodiscard]] inline BinaryResult captured_single_add(std::uint32_t left,
	std::uint32_t right, std::uint32_t fpcr) noexcept
{
	const auto left_value = std::bit_cast<float>(left);
	const auto right_value = std::bit_cast<float>(right);
	const auto captured = run_with_local_fpcr(host_fpcr(fpcr), [&] {
		float result;
		__asm__ volatile("fadd %s0, %s1, %s2" : "=w"(result) :
			"w"(left_value), "w"(right_value));
		return result;
	});
	return {std::bit_cast<std::uint32_t>(captured.value), captured.fpsr};
}

[[nodiscard]] inline BinaryResult single_add(std::uint32_t left,
	std::uint32_t right, std::uint32_t fpcr) noexcept
{
	const bool alternative = (fpcr & 2U) != 0U;
	const bool flush_input = (fpcr & 1U) != 0U;
	const bool flush_output = (fpcr & (1U << 24)) != 0U;
	if (!alternative) {
		if (flush_input && !flush_output) {
			if (is_subnormal<std::uint32_t>(left))
				left &= 0x8000'0000U;
			if (is_subnormal<std::uint32_t>(right))
				right &= 0x8000'0000U;
			return captured_single_add(left, right, fpcr & ~1U);
		}
		return captured_single_add(left, right, fpcr);
	}

	const bool left_denormal = is_subnormal<std::uint32_t>(left);
	const bool right_denormal = is_subnormal<std::uint32_t>(right);
	if (flush_input) {
		if (left_denormal)
			left &= 0x8000'0000U;
		if (right_denormal)
			right &= 0x8000'0000U;
	}
	const bool left_nan = is_nan<std::uint32_t>(left);
	const bool right_nan = is_nan<std::uint32_t>(right);
	if (left_nan || right_nan) {
		const bool any_signaling = is_signaling_nan<std::uint32_t>(left) ||
			is_signaling_nan<std::uint32_t>(right);
		const auto selected = left_nan ? left : right;
		return {process_nan(selected, fpcr), any_signaling ? 1U : 0U};
	}
	if (is_infinity<std::uint32_t>(left) && is_infinity<std::uint32_t>(right) &&
	    ((left ^ right) & 0x8000'0000U) != 0U)
		return {default_nan<std::uint32_t>(fpcr), 1U};

	auto result = captured_single_add(left, right,
		fpcr & ~(2U | 1U | (1U << 24)));
	if (!flush_input && (left_denormal || right_denormal))
		result.exceptions |= 1U << 7;
	const auto bits = static_cast<std::uint32_t>(result.bits);
	if (is_subnormal<std::uint32_t>(bits)) {
		if (flush_output) {
			result.bits = bits & 0x8000'0000U;
			result.exceptions |= (1U << 3) | (1U << 4);
		}
	} else {
		result.exceptions &= ~(1U << 3);
	}
	return result;
}

[[nodiscard]] inline BinaryResult finite_half_dot(std::uint16_t left_a,
	std::uint16_t left_b, std::uint16_t right_a, std::uint16_t right_b,
	std::uint32_t fpcr) noexcept
{
	const auto expand = [fpcr](std::uint16_t value) {
		return half_fused_long_detail::expand_half(value, fpcr);
	};
	const auto la = std::bit_cast<float>(expand(left_a));
	const auto lb = std::bit_cast<float>(expand(left_b));
	const auto ra = std::bit_cast<float>(expand(right_a));
	const auto rb = std::bit_cast<float>(expand(right_b));
	const std::uint64_t local_fpcr =
		host_fpcr(fpcr) & ~(2U | 1U | (1U << 24));
	const auto captured = run_with_local_fpcr(local_fpcr, [&] {
		float product_b;
		float result;
		__asm__ volatile("fmul %s0, %s1, %s2" : "=w"(product_b) :
			"w"(lb), "w"(rb));
		__asm__ volatile("fmadd %s0, %s1, %s2, %s3" : "=w"(result) :
			"w"(la), "w"(ra), "w"(product_b));
		return result;
	});
	return {std::bit_cast<std::uint32_t>(captured.value), captured.fpsr};
}

[[nodiscard]] inline BinaryResult half_dot(std::uint16_t left_a,
	std::uint16_t left_b, std::uint16_t right_a, std::uint16_t right_b,
	std::uint32_t fpcr) noexcept
{
	if ((fpcr & (1U << 19)) != 0U) {
		auto flush = [](std::uint16_t value) {
			return is_subnormal<std::uint16_t>(value) ?
				static_cast<std::uint16_t>(value & 0x8000U) : value;
		};
		left_a = flush(left_a);
		left_b = flush(left_b);
		right_a = flush(right_a);
		right_b = flush(right_b);
	}
	const std::array values{left_a, left_b, right_a, right_b};
	for (bool signaling : {true, false}) {
		for (const auto value : values) {
			if (is_nan<std::uint16_t>(value) &&
			    is_signaling_nan<std::uint16_t>(value) == signaling)
				return {convert_half_nan(value, fpcr), signaling ? 1U : 0U};
		}
	}
	const bool zero_a = is_zero<std::uint16_t>(left_a) || is_zero<std::uint16_t>(right_a);
	const bool zero_b = is_zero<std::uint16_t>(left_b) || is_zero<std::uint16_t>(right_b);
	const bool infinity_a = is_infinity<std::uint16_t>(left_a) || is_infinity<std::uint16_t>(right_a);
	const bool infinity_b = is_infinity<std::uint16_t>(left_b) || is_infinity<std::uint16_t>(right_b);
	const bool sign_a = ((left_a ^ right_a) & 0x8000U) != 0U;
	const bool sign_b = ((left_b ^ right_b) & 0x8000U) != 0U;
	if ((zero_a && infinity_a) || (zero_b && infinity_b) ||
	    (infinity_a && infinity_b && sign_a != sign_b))
		return {default_nan<std::uint32_t>(fpcr), 1U};
	if (infinity_a || infinity_b)
		return {(static_cast<std::uint32_t>(
			infinity_a ? sign_a : sign_b) << 31) | 0x7f80'0000U, 0U};
	return finite_half_dot(left_a, left_b, right_a, right_b, fpcr);
}

[[nodiscard]] inline std::uint16_t read_half(
	const std::array<std::byte, 16> &source, unsigned lane) noexcept
{
	return static_cast<std::uint16_t>(decode_little_endian(
		std::span{source}.subspan(lane * 2U, 2U)));
}

[[nodiscard]] inline std::uint32_t read_single(
	const std::array<std::byte, 16> &source, unsigned lane) noexcept
{
	return static_cast<std::uint32_t>(decode_little_endian(
		std::span{source}.subspan(lane * 4U, 4U)));
}

} // namespace half_matrix_detail

[[nodiscard]] inline HalfMatrixResult half_matrix_multiply_add(
	const std::array<std::byte, 16> &addends,
	const std::array<std::byte, 16> &left,
	const std::array<std::byte, 16> &right, bool widening,
	std::uint32_t fpcr) noexcept
{
	using namespace half_matrix_detail;
	HalfMatrixResult result{};
	if (!widening) {
		for (unsigned segment = 0; segment < 2U; ++segment) {
			for (unsigned row = 0; row < 2U; ++row) {
				for (unsigned column = 0; column < 2U; ++column) {
					const unsigned lane = segment * 4U + row * 2U + column;
					const unsigned left_base = segment * 4U + row * 2U;
					const unsigned right_base = segment * 4U + column * 2U;
					const auto product_a = half_binary(read_half(left,
						left_base), read_half(right, right_base), fpcr, true);
					const auto product_b = half_binary(read_half(left,
						left_base + 1U), read_half(right, right_base + 1U),
						fpcr, true);
					const auto product_sum = half_binary(
						static_cast<std::uint16_t>(product_a.bits),
						static_cast<std::uint16_t>(product_b.bits), fpcr, false);
					const auto sum = half_binary(read_half(addends, lane),
						static_cast<std::uint16_t>(product_sum.bits), fpcr, false);
					encode_little_endian(sum.bits, std::span{result.bytes}.subspan(
						lane * 2U, 2U));
					result.exceptions |= product_a.exceptions |
						product_b.exceptions | product_sum.exceptions |
						sum.exceptions;
				}
			}
		}
		return result;
	}

	for (unsigned row = 0; row < 2U; ++row) {
		for (unsigned column = 0; column < 2U; ++column) {
			const unsigned lane = row * 2U + column;
			std::array<BinaryResult, 2> products{};
			for (unsigned pair = 0; pair < 2U; ++pair) {
				const unsigned left_base = row * 4U + pair * 2U;
				const unsigned right_base = column * 4U + pair * 2U;
				products[pair] = half_dot(read_half(left, left_base),
					read_half(left, left_base + 1U),
					read_half(right, right_base),
					read_half(right, right_base + 1U), fpcr);
			}
			const auto product_sum = single_add(
				static_cast<std::uint32_t>(products[0].bits),
				static_cast<std::uint32_t>(products[1].bits), fpcr);
			const auto sum = single_add(read_single(addends, lane),
				static_cast<std::uint32_t>(product_sum.bits), fpcr);
			encode_little_endian(sum.bits, std::span{result.bytes}.subspan(
				lane * 4U, 4U));
			result.exceptions |= products[0].exceptions |
				products[1].exceptions | product_sum.exceptions |
				sum.exceptions;
		}
	}
	return result;
}

} // namespace uml::cpu::arm64::detail
