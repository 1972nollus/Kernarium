// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_aes_test_support.hpp"

#include <array>

namespace test = arm64_simd_aes_test;

namespace {

constexpr std::array operations{
	test::Operation::encrypt_round,
	test::Operation::decrypt_round,
	test::Operation::mix_columns,
	test::Operation::inverse_mix_columns,
};

void encoding_examples()
{
	test::require(test::instruction({
			test::Operation::decrypt_round,
			{0, 1},
		}) == 0x4e28'5820U,
		"AESD encoding diagram mismatch");
	test::require(test::instruction({
			test::Operation::encrypt_round,
			{2, 3},
		}) == 0x4e28'4862U,
		"AESE encoding diagram mismatch");
	test::require(test::instruction({
			test::Operation::inverse_mix_columns,
			{4, 5},
		}) == 0x4e28'78a4U,
		"AESIMC encoding diagram mismatch");
	test::require(test::instruction({
			test::Operation::mix_columns,
			{31, 30},
		}) == 0x4e28'6bdfU,
		"AESMC encoding diagram mismatch");
}

void every_register_field_and_operation()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 0;
	for (const auto operation : operations)
		for (unsigned destination = 0; destination < 32U; ++destination)
			for (unsigned source = 0; source < 32U; ++source)
				test::check_one(harness,
					{operation, {destination, source}},
					test::state(++seed));
	constexpr std::size_t expected_cases =
		operations.size() * 32U * 32U;
	static_assert(expected_cases == 4'096U);
	test::require_engine_counts(harness, expected_cases);
	harness.print_summary("arm64-simd-block-transform-allocation");
}

} // namespace

int main()
{
	encoding_examples();
	every_register_field_and_operation();
	return 0;
}
