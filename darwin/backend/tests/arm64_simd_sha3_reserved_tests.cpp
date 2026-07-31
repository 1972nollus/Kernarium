// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_sha3_test_support.hpp"

#include <array>

namespace test = arm64_simd_sha3_test;

namespace {

constexpr std::array register_cases{
	test::Registers{0, 1, 2, 3},
	test::Registers{1, 1, 2, 3},
	test::Registers{2, 1, 2, 3},
	test::Registers{3, 1, 2, 3},
	test::Registers{1, 1, 1, 1},
	test::Registers{31, 30, 29, 28},
	test::Registers{30, 31, 30, 29},
	test::Registers{31, 31, 31, 31},
};

[[nodiscard]] constexpr std::uint32_t registers3(
	std::uint32_t base, test::Registers registers) noexcept
{
	return base | ((registers.second & 31U) << 16) |
		((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t registers4(
	std::uint32_t base, test::Registers registers) noexcept
{
	return registers3(base, registers) |
		((registers.third & 31U) << 10);
}

[[nodiscard]] constexpr std::uint32_t registers2(
	std::uint32_t base, test::Registers registers) noexcept
{
	return base | ((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

void encoding_examples()
{
	test::require(test::instruction({
			test::Operation::three_way_exclusive,
			{0, 1, 2, 3},
			0,
		}) == 0xce02'0c20U,
		"EOR3 encoding diagram mismatch");
	test::require(test::instruction({
			test::Operation::clear_and_exclusive,
			{4, 5, 6, 7},
			0,
		}) == 0xce26'1ca4U,
		"BCAX encoding diagram mismatch");
	test::require(test::instruction({
			test::Operation::rotate_one_and_exclusive,
			{8, 9, 10, 0},
			0,
		}) == 0xce6a'8d28U,
		"RAX1 encoding diagram mismatch");
	test::require(test::instruction({
			test::Operation::exclusive_and_rotate,
			{11, 12, 13, 0},
			63,
		}) == 0xce8d'fd8bU,
		"XAR encoding diagram mismatch");
}

void every_neighboring_reserved_partition(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 0;
	for (const auto registers : register_cases) {
		harness.check_illegal("simd-reserved-three-register",
			registers3(0xce60'cc00U, registers),
			test::state(++seed));
		harness.check_illegal("simd-reserved-four-register",
			registers4(0xce60'0000U, registers),
			test::state(++seed));
		harness.check_illegal("simd-reserved-two-register-a",
			registers2(0xcec0'8800U, registers),
			test::state(++seed));
		harness.check_illegal("simd-reserved-two-register-b",
			registers2(0xcec0'8c00U, registers),
			test::state(++seed));
	}
}

} // namespace

int main()
{
	encoding_examples();
	test::engine_tests::EngineHarness harness;
	every_neighboring_reserved_partition(harness);
	constexpr std::size_t expected_cases = register_cases.size() * 4U;
	static_assert(expected_cases == 32U);
	test::require_engine_counts(harness, 0U, expected_cases);
	harness.print_summary("arm64-simd-logical-rotation-reserved");
	return 0;
}
