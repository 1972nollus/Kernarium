// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t three(
	unsigned operation, unsigned destination, unsigned first, unsigned second)
{
	return 0x5e00'0000U | ((second & 31U) << 16) |
		((operation & 7U) << 12) | ((first & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t two(
	unsigned operation, unsigned destination, unsigned source)
{
	return 0x5e28'0800U | ((operation & 3U) << 12) |
		((source & 31U) << 5) | (destination & 31U);
}

void every_operation_and_register_alias_shape()
{
	test::Harness harness;
	for (const unsigned destination : {0U, 1U, 15U, 31U})
		for (const unsigned first : {0U, 1U, 15U, 31U})
			for (const unsigned second : {0U, 1U, 15U, 31U})
				for (unsigned operation = 0; operation < 7; ++operation)
					harness.require_differential(
						three(operation, destination, first, second),
						test::state(operation + destination + first + second));
	for (const unsigned destination : {0U, 1U, 15U, 31U})
		for (const unsigned source : {0U, 1U, 15U, 31U})
			for (unsigned operation = 0; operation < 3; ++operation)
				harness.require_differential(two(operation, destination, source),
					test::state(operation + destination + source));
}

void reserved_opcodes_are_illegal_without_side_effects()
{
	test::Harness harness;
	for (const unsigned destination : {0U, 31U})
		for (const unsigned source : {0U, 31U}) {
			harness.require_illegal(three(7U, destination, source, 17U),
				test::state(destination + source));
			harness.require_illegal(two(3U, destination, source),
				test::state(destination + source + 1U));
		}
}

} // namespace

int main()
{
	every_operation_and_register_alias_shape();
	reserved_opcodes_are_illegal_without_side_effects();
	return 0;
}
