// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_test_support.hpp"

namespace test = arm64_fp8_test;

namespace {

void exact_formats_and_halves()
{
	test::common::Harness harness;
	for (const bool e4 : {false, true})
		for (const bool bfloat : {false, true})
			for (const bool source2 : {false, true})
				for (const bool upper : {false, true}) {
					auto initial = test::common::state(10 + e4 + bfloat * 2 +
						source2 * 4 + upper * 8);
					initial.fpmr = test::fpmr(source2 ? 0U : e4,
						source2 ? e4 : 0U);
					const unsigned offset = upper ? 8U : 0U;
					const std::array<std::uint8_t, 4> values = e4 ?
						std::array<std::uint8_t, 4>{0x38, 0xc0, 0x30, 0x3c} :
						std::array<std::uint8_t, 4>{0x3c, 0xc0, 0x38, 0x3e};
					for (unsigned i = 0; i < 8U; ++i)
						test::set_byte(initial, 1, offset + i, values[i & 3U]);
					const auto actual = test::run(test::convert_up(bfloat,
						source2, upper), initial, harness);
					const std::array<std::uint16_t, 4> expected = bfloat ?
						std::array<std::uint16_t, 4>{0x3f80, 0xc000, 0x3f00, 0x3fc0} :
						std::array<std::uint16_t, 4>{0x3c00, 0xc000, 0x3800, 0x3e00};
					for (unsigned i = 0; i < 8U; ++i)
						test::common::require(test::lane(actual, 0, i, 16) ==
							expected[i & 3U]);
				}
}

void scaling_nan_and_infinity()
{
	test::common::Harness harness;
	auto initial = test::common::state(31);
	initial.fpsr = 0;
	initial.fpmr = test::fpmr(0, 0, 0, 1);
	for (unsigned i = 0; i < 8U; ++i)
		test::set_byte(initial, 1, i, 0x3c);
	auto actual = test::run(test::convert_up(false, false, false), initial, harness);
	for (unsigned i = 0; i < 8U; ++i)
		test::common::require(test::lane(actual, 0, i, 16) == 0x3800U);

	initial.fpmr = 0;
	initial.fpsr = 0;
	test::set_byte(initial, 1, 0, 0x7d); // E5M2 SNaN
	test::set_byte(initial, 1, 1, 0x7e); // E5M2 QNaN
	test::set_byte(initial, 1, 2, 0x7c); // infinity
	test::set_byte(initial, 1, 3, 0xfc);
	actual = test::run(test::convert_up(false, false, false), initial, harness);
	test::common::require(test::lane(actual, 0, 0, 16) == 0x7e00U);
	test::common::require(test::lane(actual, 0, 1, 16) == 0x7e00U);
	test::common::require(test::lane(actual, 0, 2, 16) == 0x7c00U);
	test::common::require(test::lane(actual, 0, 3, 16) == 0xfc00U);
	test::common::require((actual.fpsr & 1U) != 0);
}

void e4_maximum_and_unsupported_format()
{
	test::common::Harness harness;
	auto initial = test::common::state(40);
	initial.fpmr = test::fpmr(1);
	test::set_byte(initial, 1, 0, 0x7e);
	test::set_byte(initial, 1, 1, 0x7f);
	const auto maximum = test::run(test::convert_up(false, false, false),
		initial, harness);
	test::common::require(test::lane(maximum, 0, 0, 16) == 0x5f00U);
	test::common::require(test::lane(maximum, 0, 1, 16) == 0x7e00U);

	initial.fpmr = test::fpmr(2);
	initial.fpsr = 0;
	const auto unsupported = test::run(test::convert_up(true, false, false),
		initial, harness);
	for (unsigned lane = 0; lane < 8U; ++lane)
		test::common::require(test::lane(unsupported, 0, lane, 16) == 0x7fc0U);
	test::common::require((unsupported.fpsr & 1U) != 0);
}

} // namespace

int main()
{
	exact_formats_and_halves();
	scaling_nan_and_infinity();
	e4_maximum_and_unsupported_format();
}
