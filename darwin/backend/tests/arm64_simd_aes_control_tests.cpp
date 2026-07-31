// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_aes_test_support.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace test = arm64_simd_aes_test;

namespace {

constexpr std::array operations{
	test::Operation::encrypt_round,
	test::Operation::decrypt_round,
	test::Operation::mix_columns,
	test::Operation::inverse_mix_columns,
};

constexpr std::array register_cases{
	test::Registers{0, 1},
	test::Registers{1, 1},
	test::Registers{31, 30},
	test::Registers{31, 31},
};

constexpr std::array<std::uint32_t, 8> fpcr_values{
	0x0000'0000U,
	0x0040'0000U,
	0x0080'0000U,
	0x00c0'0000U,
	0x0100'0000U,
	0x0200'0000U,
	0x0100'9f00U,
	0x02c0'9f00U,
};

constexpr std::array<std::uint32_t, 8> fpsr_values{
	0x0000'0000U,
	0x0000'0001U,
	0x0000'001fU,
	0x0000'0080U,
	0x0000'009fU,
	0x0800'0000U,
	0x0800'009fU,
	0xf800'009fU,
};

void every_control_state(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 0;
	for (const auto operation : operations)
		for (const auto registers : register_cases)
			for (const auto fpcr : fpcr_values)
				for (const auto fpsr : fpsr_values) {
					auto initial = test::state(++seed);
					initial.fpcr = fpcr;
					initial.fpsr = fpsr;
					initial.nzcv =
						(seed & 15U) << 28U;
					initial.pstate_dit =
						(seed & 1U) != 0;
					test::check_one(harness,
						{operation, registers},
						initial);
				}
}

void run_matrix(test::engine_tests::EngineHarnessOptions options,
	std::string_view summary)
{
	test::engine_tests::EngineHarness harness{options};
	every_control_state(harness);
	constexpr std::size_t expected_cases =
		operations.size() * register_cases.size() *
		fpcr_values.size() * fpsr_values.size();
	static_assert(expected_cases == 1'024U);
	test::require_engine_counts(harness, expected_cases);
	harness.print_summary(summary);
}

} // namespace

int main()
{
	run_matrix({}, "arm64-simd-block-transform-control");
	run_matrix(
		{.force_software_aes = true},
		"arm64-simd-block-transform-control-portable");
	return 0;
}
