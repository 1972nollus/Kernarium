// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace test = arm64_simd_test;

namespace {

struct Registers final {
	unsigned destination;
	unsigned source;
};

constexpr std::array register_cases{
	Registers{0, 1}, Registers{1, 1}, Registers{1, 0}, Registers{31, 30},
};

[[nodiscard]] constexpr std::uint32_t estimate(bool q, bool reciprocal_sqrt,
	Registers registers)
{
	return 0x0ea1'c800U | (q ? 0x4000'0000U : 0U) |
		(reciprocal_sqrt ? 0x2000'0000U : 0U) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

void write_word(test::arm64::CpuState &cpu, unsigned reg, unsigned lane,
	std::uint32_t value)
{
	for (unsigned byte = 0; byte < 4; ++byte)
		cpu.vectors[reg].bytes[lane * 4U + byte] = static_cast<std::byte>(
			value >> (byte * 8U));
}

[[nodiscard]] std::uint32_t read_word(const test::arm64::CpuState &cpu,
	unsigned reg, unsigned lane)
{
	std::uint32_t value = 0;
	for (unsigned byte = 0; byte < 4; ++byte)
		value |= std::to_integer<std::uint32_t>(
			cpu.vectors[reg].bytes[lane * 4U + byte]) << (byte * 8U);
	return value;
}

[[nodiscard]] constexpr std::uint32_t reciprocal(unsigned top_nine)
{
	if (top_nine < 256U)
		return 0xffff'ffffU;
	const unsigned a = top_nine * 2U + 1U;
	const unsigned b = (1U << 19) / a;
	return ((b + 1U) / 2U) << 23;
}

[[nodiscard]] constexpr std::uint32_t reciprocal_sqrt(unsigned top_nine)
{
	if (top_nine < 128U)
		return 0xffff'ffffU;
	unsigned a = top_nine;
	if (a < 256U)
		a = a * 2U + 1U;
	else
		a = ((a >> 1) * 2U + 1U) * 2U;
	unsigned b = 512U;
	while (static_cast<std::uint64_t>(a) * (b + 1U) * (b + 1U) <
	       (std::uint64_t{1} << 28))
		++b;
	return ((b + 1U) / 2U) << 23;
}

void every_input_bin_and_ignored_low_bit()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (const bool rsqrt : {false, true})
			for (unsigned top_nine = 0; top_nine < 512; ++top_nine) {
				auto initial = test::state(top_nine);
				initial.vectors[1].bytes.fill(std::byte{});
				const unsigned lanes = q ? 4U : 2U;
				for (unsigned lane = 0; lane < lanes; ++lane)
					write_word(initial, 1, lane, (top_nine << 23) |
						(lane & 1U ? 0x007f'ffffU : 0U));
				const auto instruction = estimate(q, rsqrt, Registers{0, 1});
				const auto result = harness.run_interpreter(instruction, initial);
				const auto expected = rsqrt ? reciprocal_sqrt(top_nine) :
					reciprocal(top_nine);
				for (unsigned lane = 0; lane < lanes; ++lane)
					test::require(read_word(result, 0, lane) == expected);
				for (unsigned byte = lanes * 4U; byte < 16; ++byte)
					test::require(result.vectors[0].bytes[byte] == std::byte{});
				harness.require_differential(instruction, initial);
			}
}

void register_aliases()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (const bool rsqrt : {false, true})
			for (const auto registers : register_cases)
				for (const unsigned seed : {0U, 127U, 128U, 255U, 256U, 511U}) {
					auto initial = test::state(seed);
					write_word(initial, registers.source, 0, seed << 23);
					harness.require_differential(estimate(q, rsqrt, registers),
						initial);
				}
}

} // namespace

int main()
{
	every_input_bin_and_ignored_low_bit();
	register_aliases();
	return 0;
}
