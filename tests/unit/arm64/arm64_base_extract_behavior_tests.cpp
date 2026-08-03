// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_extract_test_support.hpp"

#include <array>

namespace test = arm64_base_extract_test;

namespace {

void every_shift_and_value_pair(test::engine_tests::EngineHarness &harness)
{
	const std::array values{
		std::array{std::uint64_t{0}, std::uint64_t{0}},
		std::array{~std::uint64_t{0}, std::uint64_t{0}},
		std::array{std::uint64_t{0}, ~std::uint64_t{0}},
		std::array{std::uint64_t{0x0123'4567'89ab'cdefULL},
			std::uint64_t{0xfedc'ba98'7654'3210ULL}},
		std::array{std::uint64_t{0x8000'0000'0000'0001ULL},
			std::uint64_t{0xaaaa'5555'aaaa'5555ULL}},
	};
	unsigned seed = 40'000U;
	for (const bool wide : {false, true}) {
		const unsigned width = wide ? 64U : 32U;
		for (unsigned shift = 0; shift < width; ++shift)
			for (const auto &value : values) {
				auto initial = test::state(++seed);
				initial.registers[1] = value[0];
				initial.registers[2] = value[1];
				test::check(harness, "base-extract-behavior", wide,
					shift, 1U, 2U, 7U, initial);

				initial.registers[1] = value[1];
				test::check(harness, "base-ror-behavior", wide,
					shift, 1U, 1U, 7U, initial);
			}
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_shift_and_value_pair(harness);
	harness.print_summary("arm64-base-extract-behavior");
	return 0;
}
