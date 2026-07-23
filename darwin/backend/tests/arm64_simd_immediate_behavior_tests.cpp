// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <cstdint>
#include <cstring>

namespace test = arm64_simd_test;

namespace {

struct Decoded final {
	std::uint64_t pattern;
	unsigned action;
};

[[nodiscard]] constexpr std::uint32_t modified_immediate(bool q, bool op,
	unsigned mode, bool o2, unsigned immediate, unsigned destination)
{
	return 0x0f00'0400U | (q ? 0x4000'0000U : 0U) |
		(op ? 0x2000'0000U : 0U) | ((immediate & 0xe0U) << 11) |
		((mode & 0xfU) << 12) | (o2 ? 0x800U : 0U) |
		((immediate & 0x1fU) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr bool valid(bool q, bool op, unsigned mode, bool o2)
{
	if (o2)
		return !op && mode == 15U;
	return !op || mode != 15U || q;
}

[[nodiscard]] constexpr std::uint64_t replicate_32(std::uint32_t value)
{
	return value | (static_cast<std::uint64_t>(value) << 32);
}

[[nodiscard]] constexpr Decoded decode(bool op, unsigned mode, bool o2,
	unsigned immediate)
{
	if (o2) {
		const std::uint64_t sign = immediate >> 7;
		const std::uint64_t exponent_bit = (immediate >> 6) & 1U;
		const std::uint64_t fp = (sign << 15) |
			((1U - exponent_bit) << 14) |
			(exponent_bit * 0x3000U) |
			(static_cast<std::uint64_t>(immediate & 0x3fU) << 6);
		return {fp * 0x0001'0001'0001'0001ULL, 0U};
	}
	const bool combine = (mode & 1U) != 0 && mode != 13U && mode != 15U;
	std::uint64_t pattern = 0;
	if (mode <= 7U) {
		const unsigned base = combine ? mode - 1U : mode;
		const auto element = static_cast<std::uint64_t>(immediate) <<
			((base >> 1) * 8);
		pattern = replicate_32(static_cast<std::uint32_t>(element));
	} else if (mode <= 11U) {
		const unsigned base = combine ? mode - 1U : mode;
		const auto element = static_cast<std::uint64_t>(immediate) <<
			(((base >> 1) & 1U) * 8);
		pattern = element | (element << 16) | (element << 32) |
			(element << 48);
	} else if (mode == 12U) {
		const auto element = (static_cast<std::uint64_t>(immediate) << 8) |
			0xffU;
		pattern = element | (element << 32);
	} else if (mode == 13U) {
		const auto element = (static_cast<std::uint64_t>(immediate) << 16) |
			0xffffU;
		pattern = element | (element << 32);
	} else if (mode == 14U && !op) {
		pattern = static_cast<std::uint64_t>(immediate) *
			0x0101'0101'0101'0101ULL;
	} else if (mode == 14U) {
		for (unsigned index = 0; index < 8; ++index)
			if ((immediate & (1U << index)) != 0)
				pattern |= 0xffULL << (index * 8);
	} else if (!op) {
		const std::uint32_t sign = immediate >> 7;
		const std::uint32_t exponent_bit = (immediate >> 6) & 1U;
		const std::uint32_t fraction = immediate & 0x3fU;
		const std::uint32_t exponent = ((1U - exponent_bit) << 7) |
			(exponent_bit * 0x7cU) | ((fraction >> 4) & 3U);
		const std::uint32_t fp = (sign << 31) | (exponent << 23) |
			((fraction & 15U) << 19);
		pattern = replicate_32(fp);
	} else {
		const std::uint64_t sign = immediate >> 7;
		const std::uint64_t exponent_bit = (immediate >> 6) & 1U;
		const std::uint64_t fraction = immediate & 0x3fU;
		const std::uint64_t exponent = ((1U - exponent_bit) << 10) |
			(exponent_bit * 0x3fcU) | ((fraction >> 4) & 3U);
		pattern = (sign << 63) | (exponent << 52) |
			((fraction & 15U) << 48);
	}
	if (!combine && op && mode != 14U && mode != 15U)
		pattern = ~pattern;
	return {pattern, combine ? (op ? 2U : 1U) : 0U};
}

[[nodiscard]] std::uint64_t half(
	const test::arm64::VectorRegister &vector, unsigned index)
{
	std::uint64_t result;
	std::memcpy(&result, vector.bytes.data() + index * sizeof(result),
		sizeof(result));
	return result;
}

void every_immediate_and_variant()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (const bool op : {false, true})
			for (unsigned mode = 0; mode < 16; ++mode)
				for (const bool o2 : {false, true}) {
					if (!valid(q, op, mode, o2))
						continue;
					for (unsigned immediate = 0; immediate < 256; ++immediate) {
						const unsigned destination =
							(immediate & 1U) != 0 ? 0U : 31U;
						auto initial = test::state(
							immediate + mode * 7U + op * 19U);
						const auto old_low = half(
							initial.vectors[destination], 0);
						const auto old_high = half(
							initial.vectors[destination], 1);
						const auto expected = decode(op, mode, o2, immediate);
						const auto instruction = modified_immediate(
							q, op, mode, o2, immediate, destination);
						const auto result = harness.run_interpreter(
							instruction, initial);
						const auto expected_low = expected.action == 0U ?
							expected.pattern : expected.action == 1U ?
							old_low | expected.pattern :
							old_low & ~expected.pattern;
						const auto expected_high = !q ? 0U :
							expected.action == 0U ? expected.pattern :
							expected.action == 1U ?
							old_high | expected.pattern :
							old_high & ~expected.pattern;
						test::require(half(result.vectors[destination], 0) ==
							expected_low);
						test::require(half(result.vectors[destination], 1) ==
							expected_high);
						harness.require_differential(instruction, initial);
					}
				}
}

void characteristic_known_answers()
{
	test::require(decode(false, 0, false, 0x81).pattern ==
		0x0000'0081'0000'0081ULL);
	test::require(decode(false, 13, false, 0x12).pattern ==
		0x0012'ffff'0012'ffffULL);
	test::require(decode(true, 14, false, 0x81).pattern ==
		0xff00'0000'0000'00ffULL);
	test::require(decode(false, 15, true, 0x00).pattern ==
		0x4000'4000'4000'4000ULL);
	test::require(decode(false, 15, false, 0x00).pattern ==
		0x4000'0000'4000'0000ULL);
	test::require(decode(true, 15, false, 0x00).pattern ==
		0x4000'0000'0000'0000ULL);
}

} // namespace

int main()
{
	characteristic_known_answers();
	every_immediate_and_variant();
	return 0;
}
