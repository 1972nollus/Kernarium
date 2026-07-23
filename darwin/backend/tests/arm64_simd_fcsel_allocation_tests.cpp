// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcsel_test_support.hpp"

namespace test = arm64_simd_fcsel_test;
namespace base = arm64_simd_test;

namespace {

void every_precision_and_condition_encoding()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U})
		for (unsigned condition = 0; condition < 16U; ++condition) {
			auto initial = base::state(bytes + condition);
			initial.nzcv = (condition ^ 9U) << 28;
			initial.fpsr = 0x0800'0090U;
			test::require_result(test::instruction(bytes, condition,
				28U, 29U, 30U), bytes, 28U, 29U, 30U, initial, harness);
		}
}

void reserved_ftype_is_early_illegal()
{
	base::Harness harness;
	const auto encoded = 0x1e20'0c00U | (2U << 22) | (30U << 16) |
		(13U << 12) | (29U << 5) | 28U;
	harness.require_illegal(encoded, base::state(44U));
}

} // namespace

int main()
{
	every_precision_and_condition_encoding();
	reserved_ftype_is_early_illegal();
	return 0;
}
