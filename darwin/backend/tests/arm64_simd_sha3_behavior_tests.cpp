// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

namespace test = arm64_simd_test;

namespace {

struct ThreeRegisters final {
	unsigned destination;
	unsigned first;
	unsigned second;
	unsigned third;
};

struct TwoRegisters final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

constexpr std::array three_register_cases{
	ThreeRegisters{0, 1, 2, 3}, ThreeRegisters{1, 1, 2, 3},
	ThreeRegisters{2, 1, 2, 3}, ThreeRegisters{3, 1, 2, 3},
	ThreeRegisters{1, 1, 1, 1}, ThreeRegisters{31, 30, 29, 28},
};
constexpr std::array two_register_cases{
	TwoRegisters{0, 1, 2}, TwoRegisters{1, 1, 2},
	TwoRegisters{2, 1, 2}, TwoRegisters{1, 1, 1},
	TwoRegisters{31, 30, 29},
};

[[nodiscard]] constexpr std::uint32_t crypto_three(bool bcax,
	ThreeRegisters registers)
{
	return (bcax ? 0xce20'0000U : 0xce00'0000U) |
		((registers.second & 31U) << 16) |
		((registers.third & 31U) << 10) |
		((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t rax1(TwoRegisters registers)
{
	return 0xce60'8c00U | ((registers.second & 31U) << 16) |
		((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t xar(unsigned rotate,
	TwoRegisters registers)
{
	return 0xce80'0000U | ((registers.second & 31U) << 16) |
		((rotate & 63U) << 10) | ((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] std::uint64_t lane(
	const test::arm64::VectorRegister &vector, unsigned index)
{
	std::uint64_t result;
	std::memcpy(&result, vector.bytes.data() + index * sizeof(result),
		sizeof(result));
	return result;
}

void eor3_and_bcax()
{
	test::Harness harness;
	for (const bool bcax : {false, true})
		for (const auto registers : three_register_cases)
			for (unsigned seed = 0; seed < 16; ++seed) {
				const auto initial = test::state(seed + bcax * 23U);
				const auto instruction = crypto_three(bcax, registers);
				const auto result = harness.run_interpreter(instruction, initial);
				for (unsigned byte = 0; byte < 16; ++byte) {
					const auto first = initial.vectors[registers.first].bytes[byte];
					const auto second = initial.vectors[registers.second].bytes[byte];
					const auto third = initial.vectors[registers.third].bytes[byte];
					const auto expected = bcax ? first ^ (second & ~third) :
						first ^ second ^ third;
					test::require(result.vectors[registers.destination]
						.bytes[byte] == expected);
				}
				harness.require_differential(instruction, initial);
			}
}

void rotate_forms()
{
	test::Harness harness;
	for (const auto registers : two_register_cases)
		for (unsigned seed = 0; seed < 16; ++seed) {
			const auto initial = test::state(seed + 71U);
			const auto instruction = rax1(registers);
			const auto result = harness.run_interpreter(instruction, initial);
			for (unsigned index = 0; index < 2; ++index)
				test::require(lane(result.vectors[registers.destination], index) ==
					(lane(initial.vectors[registers.first], index) ^
					 std::rotl(lane(initial.vectors[registers.second], index), 1)));
			harness.require_differential(instruction, initial);
		}
	for (unsigned rotate = 0; rotate < 64; ++rotate)
		for (const auto registers : two_register_cases) {
			const auto initial = test::state(rotate * 3U + registers.destination);
			const auto instruction = xar(rotate, registers);
			const auto result = harness.run_interpreter(instruction, initial);
			for (unsigned index = 0; index < 2; ++index)
				test::require(lane(result.vectors[registers.destination], index) ==
					std::rotr(lane(initial.vectors[registers.first], index) ^
						lane(initial.vectors[registers.second], index),
						static_cast<int>(rotate)));
			harness.require_differential(instruction, initial);
		}
}

} // namespace

int main()
{
	eor3_and_bcax();
	rotate_forms();
	return 0;
}
