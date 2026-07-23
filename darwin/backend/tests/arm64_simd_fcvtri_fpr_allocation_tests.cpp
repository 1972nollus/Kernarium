// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

struct Widths final {
	unsigned source;
	unsigned integer;
};

inline constexpr std::array widths{
	Widths{16U, 32U}, Widths{16U, 64U},
	Widths{32U, 64U}, Widths{64U, 32U},
};

void every_width_rounding_signedness_alias_and_nep()
{
	base::Harness harness;
	for (const auto width : widths)
		for (const auto rounding : test::roundings)
			for (const bool unsigned_result : {false, true})
				for (const unsigned destination : {28U, 29U}) {
					auto initial = base::state(width.source + width.integer +
						static_cast<unsigned>(rounding) + destination);
					initial.fpcr = 4U;
					test::set_gpr_source(initial, 29U, width.source,
						test::precision::one(width.source));
					test::require_fpr(test::fpr_instruction(rounding,
						unsigned_result, width.source, width.integer,
						destination, 29U), width.integer, destination,
						1U, 0U, initial, harness);
				}
}

} // namespace

int main()
{
	every_width_rounding_signedness_alias_and_nep();
	return 0;
}
