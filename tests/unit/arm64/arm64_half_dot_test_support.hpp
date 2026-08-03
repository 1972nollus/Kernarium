// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_bfloat_arithmetic_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace arm64_half_dot_test {

namespace exact = arm64_bfloat_arithmetic_test;
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

inline constexpr std::uint32_t fpcr_fiz = 1U << 0U;
inline constexpr std::uint32_t fpcr_ah = 1U << 1U;
inline constexpr std::uint32_t fpcr_fz16 = 1U << 19U;
inline constexpr std::uint32_t fpcr_fz = 1U << 24U;
inline constexpr std::uint32_t fpcr_dn = 1U << 25U;

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

inline constexpr std::array register_aliases{
	Registers{0U, 1U, 2U}, Registers{1U, 1U, 2U},
	Registers{2U, 1U, 2U}, Registers{1U, 2U, 2U},
	Registers{3U, 3U, 3U}, Registers{31U, 30U, 29U},
	Registers{30U, 31U, 30U},
};

struct Result final {
	std::uint32_t bits{};
	std::uint32_t flags{};
};

struct ExpectedCounts final {
	std::size_t cases{};
	std::size_t jit{};
	std::size_t feature_skips{};

	void add(engine_tests::NativeRequirements requirements,
		bool invalid = false)
	{
		++cases;
		if (invalid || engine_tests::host_supports(requirements))
			++jit;
		else
			++feature_skips;
	}
};

[[noreturn]] inline void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

inline void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

inline void require_counts(const engine_tests::EngineHarness &harness,
	const ExpectedCounts &expected)
{
	const auto &actual = harness.counts();
	require(actual.interpreter == expected.cases,
		"unexpected interpreter case count");
	require(actual.gadgets == expected.cases,
		"unexpected gadget case count");
	require(actual.jit == expected.jit,
		"unexpected JIT case count");
	require(actual.jit_skipped_host_feature == expected.feature_skips,
		"unexpected JIT feature-skip count");
	require(actual.jit_skipped == expected.feature_skips,
		"unexpected total JIT skip count");
	require(actual.jit_unavailable == 0U,
		"JIT engine was unavailable");
}

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	Registers registers = {0U, 1U, 2U}) noexcept
{
	const std::uint32_t base =
		form.by_element ? 0x0f40'9000U : 0x0e80'fc00U;
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
	bool q, bool u, unsigned size,
	Registers registers = {0U, 1U, 2U}) noexcept
{
	return 0x0e00'fc00U |
		(static_cast<std::uint32_t>(q) << 30U) |
		(static_cast<std::uint32_t>(u) << 29U) |
		((size & 3U) << 22U) |
		((registers.second & 31U) << 16U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements() noexcept
{
	using ::uml::cpu::ArmFeature;
	/*
	 * Native execution is enabled only when the host implements both the
	 * instruction and its complete alternative-control behavior. This is the
	 * same requirement enforced by the production JIT emitter.
	 */
	return engine_tests::NativeRequirements::features({
		ArmFeature::f16f32dot,
		ArmFeature::afp,
	});
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
{
	auto cpu = exact::state(seed);
	cpu.fpcr = 0U;
	cpu.fpsr = 0x0800'0000U;
	return cpu;
}

inline void write_half(std::array<std::byte, 16> &bytes,
	unsigned lane, std::uint16_t bits)
{
	bytes[lane * 2U] = static_cast<std::byte>(bits & 0xffU);
	bytes[lane * 2U + 1U] = static_cast<std::byte>(bits >> 8U);
}

[[nodiscard]] inline std::uint16_t read_half(
	const std::array<std::byte, 16> &bytes, unsigned lane)
{
	return static_cast<std::uint16_t>(
		std::to_integer<unsigned>(bytes[lane * 2U]) |
		(std::to_integer<unsigned>(bytes[lane * 2U + 1U]) << 8U));
}

[[nodiscard]] constexpr std::uint16_t half(float value) noexcept
{
	return std::bit_cast<std::uint16_t>(static_cast<_Float16>(value));
}

[[nodiscard]] constexpr bool single_subnormal(
	std::uint32_t bits) noexcept
{
	return (bits & 0x7f80'0000U) == 0U &&
		(bits & 0x007f'ffffU) != 0U;
}

[[nodiscard]] constexpr std::uint32_t default_nan(
	std::uint32_t fpcr) noexcept
{
	return (fpcr & fpcr_ah) != 0U ? 0xffc0'0000U : 0x7fc0'0000U;
}

[[nodiscard]] inline exact::BinaryValue unpack_half(
	std::uint16_t bits, std::uint32_t fpcr)
{
	const bool negative = (bits >> 15U) != 0U;
	const unsigned exponent = (bits >> 10U) & 31U;
	const unsigned fraction = bits & 0x3ffU;
	if (exponent == 31U)
		return {
			fraction == 0U ? exact::FloatingClass::infinity :
				exact::FloatingClass::nan,
			negative, fraction != 0U && (fraction & 0x200U) == 0U,
			0U, 0, bits,
		};
	if (exponent == 0U) {
		if (fraction == 0U || (fpcr & fpcr_fz16) != 0U)
			return {exact::FloatingClass::finite, negative, false,
				0U, 0, bits};
		return {exact::FloatingClass::finite, negative, false,
			fraction, -24, bits};
	}
	return {exact::FloatingClass::finite, negative, false,
		0x400U | fraction,
		static_cast<int>(exponent) - 15 - 10, bits};
}

[[nodiscard]] inline Result round_single(
	const exact::ExactValue &value, std::uint32_t fpcr)
{
	if (value.magnitude.zero())
		return {
			static_cast<std::uint32_t>(value.negative) << 31U, 0U,
		};
	const unsigned source_bits = value.magnitude.bit_length();
	const int top_exponent =
		value.exponent + static_cast<int>(source_bits) - 1;
	const bool alternative = (fpcr & fpcr_ah) != 0U;
	const bool flush = (fpcr & fpcr_fz) != 0U;
	if (!alternative && flush && top_exponent < -126)
		return {
			static_cast<std::uint32_t>(value.negative) << 31U,
			exact::fpsr_underflow,
		};
	const auto rounded = exact::round_single(value,
		(fpcr >> 22U) & 3U, false, false, true);
	if (alternative && flush && single_subnormal(rounded.bits))
		return {
			rounded.bits & 0x8000'0000U,
			rounded.flags | exact::fpsr_underflow |
				exact::fpsr_inexact,
		};
	return {rounded.bits, rounded.flags};
}

[[nodiscard]] constexpr std::uint32_t convert_half_nan(
	std::uint16_t bits, std::uint32_t fpcr) noexcept
{
	if ((fpcr & fpcr_dn) != 0U)
		return default_nan(fpcr);
	return (static_cast<std::uint32_t>(bits & 0x8000U) << 16U) |
		0x7fc0'0000U |
		(static_cast<std::uint32_t>(bits & 0x01ffU) << 13U);
}

[[nodiscard]] inline Result dot_product(
	std::uint16_t left_a_bits, std::uint16_t left_b_bits,
	std::uint16_t right_a_bits, std::uint16_t right_b_bits,
	std::uint32_t fpcr)
{
	const std::array bits{
		left_a_bits, left_b_bits, right_a_bits, right_b_bits,
	};
	const std::array values{
		unpack_half(bits[0], fpcr), unpack_half(bits[1], fpcr),
		unpack_half(bits[2], fpcr), unpack_half(bits[3], fpcr),
	};
	for (const bool signaling : {true, false})
		for (unsigned index = 0U; index < values.size(); ++index)
			if (values[index].type == exact::FloatingClass::nan &&
			    values[index].signaling == signaling)
				return {
					convert_half_nan(bits[index], fpcr),
					signaling ? exact::fpsr_invalid : 0U,
				};

	const auto zero = [](const exact::BinaryValue &value) {
		return value.type == exact::FloatingClass::finite &&
			value.magnitude.zero();
	};
	const auto infinity = [](const exact::BinaryValue &value) {
		return value.type == exact::FloatingClass::infinity;
	};
	const bool zero_a = zero(values[0]) || zero(values[2]);
	const bool zero_b = zero(values[1]) || zero(values[3]);
	const bool infinity_a =
		infinity(values[0]) || infinity(values[2]);
	const bool infinity_b =
		infinity(values[1]) || infinity(values[3]);
	const bool sign_a = values[0].negative != values[2].negative;
	const bool sign_b = values[1].negative != values[3].negative;
	if ((zero_a && infinity_a) || (zero_b && infinity_b) ||
	    (infinity_a && infinity_b && sign_a != sign_b))
		return {default_nan(fpcr), exact::fpsr_invalid};
	if (infinity_a || infinity_b) {
		const bool negative = infinity_a ? sign_a : sign_b;
		return {
			(static_cast<std::uint32_t>(negative) << 31U) |
				0x7f80'0000U,
			0U,
		};
	}

	const auto product = [&](unsigned first, unsigned second,
		bool product_zero, bool negative) {
		return product_zero ?
			exact::ExactValue{negative, 0U, 0} :
			exact::multiply_exact(values[first], values[second]);
	};
	const auto first_product =
		product(0U, 2U, zero_a, sign_a);
	const auto second_product =
		product(1U, 3U, zero_b, sign_b);
	const auto sum = exact::add_exact(first_product, second_product,
		((fpcr >> 22U) & 3U) == 2U);
	return round_single(sum, fpcr);
}

[[nodiscard]] constexpr Result process_single_nan(
	std::uint32_t bits, bool signaling, std::uint32_t fpcr) noexcept
{
	return {
		(fpcr & fpcr_dn) != 0U ?
			default_nan(fpcr) : bits | 0x0040'0000U,
		signaling ? exact::fpsr_invalid : 0U,
	};
}

[[nodiscard]] inline Result add_single(std::uint32_t left_bits,
	std::uint32_t right_bits, std::uint32_t fpcr)
{
	const bool alternative = (fpcr & fpcr_ah) != 0U;
	const bool fiz = (fpcr & fpcr_fiz) != 0U;
	const bool fz = (fpcr & fpcr_fz) != 0U && !alternative;
	bool left_flushed{};
	bool right_flushed{};
	const auto left = exact::unpack_single(
		left_bits, fiz || fz, left_flushed);
	const auto right = exact::unpack_single(
		right_bits, fiz || fz, right_flushed);
	std::uint32_t flags =
		fz && (left_flushed || right_flushed) ?
			exact::fpsr_input_denormal : 0U;
	const bool left_nan = left.type == exact::FloatingClass::nan;
	const bool right_nan = right.type == exact::FloatingClass::nan;
	if (left_nan || right_nan) {
		if (alternative && left_nan && right_nan) {
			const auto result = process_single_nan(left_bits,
				left.signaling || right.signaling, fpcr);
			return {result.bits, flags | result.flags};
		}
		for (const bool signaling : {true, false}) {
			if (left_nan && left.signaling == signaling) {
				const auto result =
					process_single_nan(left_bits, signaling, fpcr);
				return {result.bits, flags | result.flags};
			}
			if (right_nan && right.signaling == signaling) {
				const auto result =
					process_single_nan(right_bits, signaling, fpcr);
				return {result.bits, flags | result.flags};
			}
		}
	}

	if (alternative && !fiz &&
	    (single_subnormal(left_bits) || single_subnormal(right_bits)))
		flags |= exact::fpsr_input_denormal;
	const bool left_infinity =
		left.type == exact::FloatingClass::infinity;
	const bool right_infinity =
		right.type == exact::FloatingClass::infinity;
	if (left_infinity && right_infinity &&
	    left.negative != right.negative)
		return {default_nan(fpcr), flags | exact::fpsr_invalid};
	if (left_infinity || right_infinity) {
		const bool negative =
			left_infinity ? left.negative : right.negative;
		return {
			(static_cast<std::uint32_t>(negative) << 31U) |
				0x7f80'0000U,
			flags,
		};
	}
	const auto sum = exact::add_exact(exact::exact(left),
		exact::exact(right), ((fpcr >> 22U) & 3U) == 2U);
	const auto rounded = round_single(sum, fpcr);
	return {rounded.bits, flags | rounded.flags};
}

[[nodiscard]] inline Result dot_add(std::uint32_t addend,
	std::uint16_t left_a, std::uint16_t left_b,
	std::uint16_t right_a, std::uint16_t right_b,
	std::uint32_t fpcr)
{
	const auto product = dot_product(
		left_a, left_b, right_a, right_b, fpcr);
	const auto result = add_single(addend, product.bits, fpcr);
	return {result.bits, product.flags | result.flags};
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, Form form, Registers registers)
{
	const auto addends =
		initial.vectors[registers.destination].bytes;
	const auto first = initial.vectors[registers.first].bytes;
	const auto second = initial.vectors[registers.second].bytes;
	auto expected = initial;
	std::array<std::byte, 16> result{};
	std::uint32_t flags{};
	const unsigned lanes = form.wide ? 4U : 2U;
	for (unsigned lane = 0U; lane < lanes; ++lane) {
		const unsigned first_pair = lane * 2U;
		const unsigned second_pair =
			form.by_element ? form.index * 2U : first_pair;
		const auto lane_result = dot_add(
			exact::read_single(addends, lane),
			read_half(first, first_pair),
			read_half(first, first_pair + 1U),
			read_half(second, second_pair),
			read_half(second, second_pair + 1U),
			initial.fpcr);
		exact::write_single(result, lane, lane_result.bits);
		flags |= lane_result.flags;
	}
	expected.vectors[registers.destination].bytes = result;
	expected.fpsr |= flags;
	return expected;
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, Form form, Registers registers,
	const arm64::CpuState &initial, ExpectedCounts &counts)
{
	const auto native = native_requirements();
	harness.check_one(name, instruction(form, registers), initial,
		expected_state(initial, form, registers), native);
	counts.add(native);
}

} // namespace arm64_half_dot_test
