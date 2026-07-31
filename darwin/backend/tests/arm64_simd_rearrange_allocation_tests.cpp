// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_rearrange_test_support.hpp"
#include "arm64_simd_test_support.hpp"

#include <array>

namespace test = arm64_simd_rearrange_test;
namespace base = arm64_simd_test;

namespace {

void encoding_examples()
{
	constexpr test::PermuteRegisters registers{0U, 1U, 2U};
	base::require(test::permute_instruction(true, 0U, 1U, registers) ==
		0x4e02'1820U); // UZP1 V0.16B, V1.16B, V2.16B
	base::require(test::permute_instruction(true, 0U, 2U, registers) ==
		0x4e02'2820U); // TRN1 V0.16B, V1.16B, V2.16B
	base::require(test::permute_instruction(true, 0U, 3U, registers) ==
		0x4e02'3820U); // ZIP1 V0.16B, V1.16B, V2.16B
	base::require(test::permute_instruction(true, 0U, 5U, registers) ==
		0x4e02'5820U); // UZP2 V0.16B, V1.16B, V2.16B
	base::require(test::permute_instruction(true, 0U, 6U, registers) ==
		0x4e02'6820U); // TRN2 V0.16B, V1.16B, V2.16B
	base::require(test::permute_instruction(true, 0U, 7U, registers) ==
		0x4e02'7820U); // ZIP2 V0.16B, V1.16B, V2.16B
	base::require(test::table_instruction(
		true, false, 2U, {0U, 4U, 1U}) == 0x4e01'2080U);
	base::require(test::table_instruction(
		false, true, 4U, {0U, 4U, 1U}) == 0x0e01'7080U);
}

void complete_allocation()
{
	test::engine_tests::EngineHarness harness;
	constexpr test::PermuteRegisters permute_registers{31, 30, 29};
	for (const bool q : {false, true})
		for (unsigned size = 0; size < 4; ++size)
			for (const auto operation : test::permute_operations) {
				const auto instruction = test::permute_instruction(
					q, size, operation.opcode, permute_registers);
				const auto initial =
					test::state(size + operation.opcode + q * 13U);
				if (test::valid_permute(q, size)) {
					harness.check_one(operation.name,
						instruction, initial,
						test::expected_permute_state(
							initial, q, size, operation.opcode,
							permute_registers),
						test::native_requirements());
				} else {
					harness.check_illegal(operation.name, instruction,
						initial, test::native_requirements());
				}
			}

	constexpr test::TableRegisters table_registers{31, 30, 29};
	for (const bool q : {false, true})
		for (const bool extend : {false, true})
			for (unsigned count = 1; count <= 4; ++count) {
				const auto initial =
					test::state(count + extend * 11U + q * 19U);
				harness.check_one(test::table_name(extend),
					test::table_instruction(
						q, extend, count, table_registers),
					initial,
					test::expected_table_state(
						initial, q, extend, count, table_registers),
					test::native_requirements());
			}
	constexpr std::size_t cases = 64U;
	base::require(harness.counts().interpreter == cases);
	base::require(harness.counts().gadgets == cases);
	base::require(
		harness.counts().jit + harness.counts().jit_skipped == cases);
	harness.print_summary("arm64-simd-rearrange-allocation");
}

} // namespace

int main()
{
	encoding_examples();
	complete_allocation();
	return 0;
}
