// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_matrix_test_support.hpp"

namespace test = arm64_simd_matrix_test;
namespace base = arm64_simd_test;

namespace {

void exact_encodings_are_distinct_and_execute()
{
	base::require(test::instruction(test::Mode::signed_signed, 0U, 1U, 2U) ==
		0x4e82'a420U);
	base::require(test::instruction(test::Mode::unsigned_unsigned, 0U, 1U, 2U) ==
		0x6e82'a420U);
	base::require(test::instruction(test::Mode::unsigned_signed, 0U, 1U, 2U) ==
		0x4e82'ac20U);
	base::Harness harness;
	for (const auto mode : {test::Mode::signed_signed,
	     test::Mode::unsigned_unsigned, test::Mode::unsigned_signed}) {
		auto initial = base::state(static_cast<unsigned>(mode));
		initial.fpsr = 0x84U;
		harness.require_differential(test::instruction(mode, 28U, 29U, 30U),
			initial);
	}
}

} // namespace

int main()
{
	exact_encodings_are_distinct_and_execute();
	return 0;
}
