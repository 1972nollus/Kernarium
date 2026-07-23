// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>
#include <cstddef>

namespace test = arm64_simd_test;

namespace {

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

constexpr std::array register_cases{
	Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
	Registers{1, 1, 1}, Registers{31, 30, 29},
};

[[nodiscard]] constexpr std::uint32_t bitwise(bool q, unsigned operation,
	Registers registers)
{
	return 0x0e20'1c00U | (q ? 0x4000'0000U : 0U) |
		((operation & 3U) << 22) | ((operation & 4U) << 27) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr unsigned expected(unsigned operation, unsigned old,
	unsigned left, unsigned right)
{
	switch (operation) {
	case 0: return left & right;
	case 1: return left & ~right;
	case 2: return left | right;
	case 3: return left | ~right;
	case 4: return left ^ right;
	case 5: return (old & left) | (~old & right);
	case 6: return (old & ~right) | (left & right);
	default: return (old & right) | (left & ~right);
	}
}

void every_operation_width_and_alias()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (unsigned operation = 0; operation < 8; ++operation)
			for (const auto registers : register_cases)
				for (unsigned seed = 0; seed < 4; ++seed)
					harness.require_differential(bitwise(q, operation, registers),
						test::state(seed + operation * 7U));
}

void known_answers_and_zeroed_upper_half()
{
	test::Harness harness;
	auto initial = test::state();
	for (unsigned byte = 0; byte < 16; ++byte) {
		initial.vectors[0].bytes[byte] = static_cast<std::byte>(0xa5U ^ byte);
		initial.vectors[1].bytes[byte] = static_cast<std::byte>(0x3cU + byte);
		initial.vectors[2].bytes[byte] = static_cast<std::byte>(0xc3U - byte);
	}
	for (const bool q : {false, true})
		for (unsigned operation = 0; operation < 8; ++operation) {
			const auto result = harness.run_interpreter(bitwise(q, operation,
				Registers{0, 1, 2}), initial);
			const unsigned bytes = q ? 16U : 8U;
			for (unsigned byte = 0; byte < bytes; ++byte) {
				const auto old = std::to_integer<unsigned>(
					initial.vectors[0].bytes[byte]);
				const auto left = std::to_integer<unsigned>(
					initial.vectors[1].bytes[byte]);
				const auto right = std::to_integer<unsigned>(
					initial.vectors[2].bytes[byte]);
				test::require(std::to_integer<unsigned>(
					result.vectors[0].bytes[byte]) ==
					(expected(operation, old, left, right) & 0xffU));
			}
			for (unsigned byte = bytes; byte < 16; ++byte)
				test::require(result.vectors[0].bytes[byte] == std::byte{});
			harness.require_differential(bitwise(q, operation,
				Registers{0, 1, 2}), initial);
		}
}

void mov_alias_is_orr_with_identical_sources()
{
	test::Harness harness;
	for (const bool q : {false, true})
		for (const auto registers : {Registers{0, 1, 1}, Registers{31, 30, 30}})
			harness.require_differential(bitwise(q, 2, registers),
				test::state(41));
}

} // namespace

int main()
{
	every_operation_width_and_alias();
	known_answers_and_zeroed_upper_half();
	mov_alias_is_orr_with_identical_sources();
	return 0;
}
