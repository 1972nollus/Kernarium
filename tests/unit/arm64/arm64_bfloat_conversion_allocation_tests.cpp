// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_conversion_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_bfloat_conversion_test;

namespace {

void every_form_register_field_alias_and_merge_mode(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	for (const auto form : test::forms)
		for (unsigned merge_mode = 0U;
		     merge_mode < (form == test::Form::scalar ? 2U : 1U);
		     ++merge_mode)
			for (unsigned destination = 0U; destination < 32U;
			     ++destination)
				for (unsigned source = 0U; source < 32U; ++source) {
					const bool merge = merge_mode != 0U;
					auto initial = test::state(++seed);
					initial.fpcr = merge ?
						test::floating::fpcr_nep : 0U;
					initial.fpsr = 0x0800'0090U;
					for (unsigned lane = 0U; lane < 4U;
					     ++lane)
						test::write_single(
							initial.vectors[source].bytes,
							lane,
							0x3f80'0000U +
								lane *
								0x0080'0000U);
					char name[192];
					std::snprintf(name, sizeof(name),
						"reduced-precision-allocation form=%u "
						"merge=%u rd=%u rn=%u",
						static_cast<unsigned>(form),
						static_cast<unsigned>(merge),
						destination, source);
					test::check(harness, name, form,
						{destination, source}, initial);
				}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_form_register_field_alias_and_merge_mode(harness);
	harness.print_summary("arm64-bfloat-conversion-allocation");
	return 0;
}
