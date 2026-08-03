// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_rearrange_test_support.hpp"
#include "arm64_simd_test_support.hpp"

#include <array>

namespace test = arm64_simd_rearrange_test;
namespace base = arm64_simd_test;

namespace {

constexpr std::array destructive_aliases{
	test::PermuteRegisters{0, 1, 2},
	test::PermuteRegisters{0, 0, 1},
	test::PermuteRegisters{0, 1, 0},
	test::PermuteRegisters{0, 1, 1},
	test::PermuteRegisters{0, 0, 0},
	test::PermuteRegisters{31, 31, 0},
	test::PermuteRegisters{31, 0, 31},
	test::PermuteRegisters{31, 31, 31},
};

void check(test::engine_tests::EngineHarness &harness, bool q,
	unsigned size, test::PermuteOperation operation,
	test::PermuteRegisters registers, unsigned seed)
{
	const auto initial = test::state(seed);
	harness.check_one(operation.name,
		test::permute_instruction(q, size, operation.opcode, registers),
		initial,
		test::expected_permute_state(
			initial, q, size, operation.opcode, registers),
		test::native_requirements());
}

void every_operation_shape_register_field_and_alias()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 0;
	for (const bool q : {false, true})
		for (unsigned size = 0; size < (q ? 4U : 3U); ++size)
			for (const auto operation : test::permute_operations) {
				/* Exhaust Rd, Rn, and Rm independently for every allocated
				 * arrangement and operation.
				 */
				for (unsigned reg = 0; reg < 32U; ++reg) {
					check(harness, q, size, operation,
						{reg, 11U, 23U}, ++seed);
					check(harness, q, size, operation,
						{7U, reg, 23U}, ++seed);
					check(harness, q, size, operation,
						{7U, 11U, reg}, ++seed);
				}
				for (const auto registers : destructive_aliases)
					check(harness, q, size, operation, registers, ++seed);
			}
	constexpr std::size_t cases =
		7U * test::permute_operations.size() * (32U * 3U + 8U);
	base::require(harness.counts().interpreter == cases);
	base::require(harness.counts().gadgets == cases);
	base::require(
		harness.counts().jit + harness.counts().jit_skipped == cases);
	harness.print_summary("arm64-simd-rearrange-permute");
}

} // namespace

int main()
{
	every_operation_shape_register_field_and_alias();
	return 0;
}
