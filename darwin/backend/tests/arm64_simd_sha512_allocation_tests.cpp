// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t three(
	std::uint32_t operation, unsigned destination, unsigned first,
	unsigned second)
{
	return operation | (second << 16) | (first << 5) | destination;
}

[[nodiscard]] constexpr std::uint32_t schedule_zero(
	unsigned destination, unsigned source)
{
	return 0xcec0'8000U | (source << 5) | destination;
}

void every_register_and_alias_allocation()
{
	test::Harness harness;
	constexpr std::array operations{0xce60'8000U, 0xce60'8400U, 0xce60'8800U};
	for (const unsigned destination : {0U, 1U, 15U, 31U})
		for (const unsigned first : {0U, 1U, 15U, 31U})
			for (const unsigned second : {0U, 1U, 15U, 31U})
				for (const auto operation : operations)
					harness.require_differential(three(operation, destination,
						first, second), test::state(destination + first + second));
	for (const unsigned destination : {0U, 1U, 15U, 31U})
		for (const unsigned source : {0U, 1U, 15U, 31U})
			harness.require_differential(schedule_zero(destination, source),
				test::state(destination + source + 53U));
}

} // namespace

int main()
{
	every_register_and_alias_allocation();
	return 0;
}
