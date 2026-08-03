// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_immediate_test_support.hpp"

namespace test = arm64_simd_immediate_test;

namespace {

static_assert(test::decode(false, 0, false, 0x81).pattern ==
	0x0000'0081'0000'0081ULL);
static_assert(test::decode(false, 13, false, 0x12).pattern ==
	0x0012'ffff'0012'ffffULL);
static_assert(test::decode(true, 14, false, 0x81).pattern ==
	0xff00'0000'0000'00ffULL);
static_assert(test::decode(false, 15, true, 0x00).pattern ==
	0x4000'4000'4000'4000ULL);
static_assert(test::decode(false, 15, false, 0x00).pattern ==
	0x4000'0000'4000'0000ULL);
static_assert(test::decode(true, 15, false, 0x00).pattern ==
	0x4000'0000'0000'0000ULL);

void every_immediate_and_variant()
{
	test::engine_tests::EngineHarness harness;
	for (const bool q : {false, true})
		for (const bool op : {false, true})
			for (unsigned mode = 0; mode < 16; ++mode)
				for (const bool o2 : {false, true}) {
					if (!test::valid(q, op, mode, o2))
						continue;
					for (unsigned immediate = 0; immediate < 256;
					     ++immediate)
						for (unsigned destination = 0; destination < 32U;
						     ++destination) {
							auto initial = test::state(
								immediate + mode * 7U + op * 19U +
								o2 * 37U + q * 53U +
								destination * 71U);
							initial.fpcr =
								((immediate & 3U) << 22) |
								((immediate & 4U) != 0 ? 1U << 25 : 0U);
							initial.fpsr = 0x0800'0080U |
								(immediate & 0x1fU);
							harness.check_one(
								"simd-immediate-behavior",
								test::instruction(q, op, mode, o2,
									immediate, destination),
								initial,
								test::expected_state(initial, q, op,
									mode, o2, immediate,
									destination),
								test::native_requirements(o2));
						}
				}
	harness.print_summary("arm64-simd-immediate-behavior");
}

} // namespace

int main()
{
	every_immediate_and_variant();
	return 0;
}
