// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtxn_test_support.hpp"

namespace test = arm64_simd_fcvtxn_test;
namespace base = arm64_simd_test;

namespace {

void every_form_alias_and_scalar_nep()
{
	base::Harness harness;
	for (const auto form : {test::Form::scalar, test::Form::vector_lower,
	     test::Form::vector_upper})
		for (const unsigned destination : {28U, 29U})
			for (const bool nep : {false, true}) {
				auto initial = base::state(destination + static_cast<unsigned>(form));
				initial.fpcr = nep ? 4U : 0U;
				const unsigned count = test::lanes(form);
				std::array<std::uint32_t, 2> outputs{};
				for (unsigned lane = 0; lane < count; ++lane) {
					test::set_source_lane(initial, 29U, lane,
						0x3ff0'0000'0000'0000ULL);
					outputs[lane] = 0x3f80'0000U;
				}
				test::require_result(test::instruction(form, destination, 29U),
					form, destination, std::span{outputs}.first(count), 0U,
					initial, harness);
			}
}

} // namespace

int main()
{
	every_form_alias_and_scalar_nep();
	return 0;
}
