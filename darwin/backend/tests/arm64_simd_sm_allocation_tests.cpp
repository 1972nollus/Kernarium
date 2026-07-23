// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t three(std::uint32_t operation,
	unsigned destination, unsigned first, unsigned second)
{
	return operation | (second << 16) | (first << 5) | destination;
}

[[nodiscard]] constexpr std::uint32_t four(unsigned destination,
	unsigned first, unsigned second, unsigned third)
{
	return 0xce40'0000U | (second << 16) | (third << 10) |
		(first << 5) | destination;
}

[[nodiscard]] constexpr std::uint32_t indexed(unsigned operation,
	unsigned index, unsigned destination, unsigned first, unsigned second)
{
	return 0xce40'8000U | (second << 16) | (index << 12) |
		(operation << 10) | (first << 5) | destination;
}

[[nodiscard]] constexpr std::uint32_t sm4e(
	unsigned destination, unsigned source)
{
	return 0xcec0'8400U | (source << 5) | destination;
}

void every_operation_index_register_and_alias_shape()
{
	test::Harness harness;
	constexpr std::array three_operations{0xce60'c000U, 0xce60'c400U,
		0xce60'c800U};
	for (const unsigned destination : {0U, 1U, 15U, 31U})
		for (const unsigned first : {0U, 1U, 15U, 31U})
			for (const unsigned second : {0U, 1U, 15U, 31U}) {
				for (const auto operation : three_operations)
					harness.require_differential(three(operation, destination,
						first, second), test::state(destination + first + second));
				for (unsigned operation = 0; operation < 4; ++operation)
					for (unsigned index = 0; index < 4; ++index)
						harness.require_differential(indexed(operation, index,
							destination, first, second),
							test::state(destination + first + second + index));
			}
	for (const unsigned destination : {0U, 1U, 15U, 31U})
		for (const unsigned first : {0U, 1U, 15U, 31U})
			for (const unsigned second : {0U, 1U, 15U, 31U})
				for (const unsigned third : {0U, 1U, 15U, 31U})
					harness.require_differential(four(destination, first, second,
						third), test::state(destination + first + second + third));
	for (const unsigned destination : {0U, 1U, 15U, 31U})
		for (const unsigned source : {0U, 1U, 15U, 31U})
			harness.require_differential(sm4e(destination, source),
				test::state(destination + source + 177U));
}

void unallocated_crypto_members_are_illegal()
{
	test::Harness harness;
	for (const auto instruction : {0xce71'cc22U, 0xce71'4622U,
		0xcec0'8a22U, 0xcec0'8e22U})
		harness.require_illegal(instruction, test::state(instruction));
}

} // namespace

int main()
{
	every_operation_index_register_and_alias_shape();
	unallocated_crypto_members_are_illegal();
	return 0;
}
