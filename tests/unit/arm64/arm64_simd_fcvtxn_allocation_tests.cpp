// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_precision_conversion_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_precision_conversion_test;

namespace {

void every_form_register_edge_alias_and_scalar_nep()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 2'000U;
	for (const auto form : test::round_odd_forms) {
		for (const auto registers : test::register_edges) {
			const unsigned controls = form.scalar ? 2U : 1U;
			for (unsigned control = 0U; control < controls; ++control) {
				auto initial = test::state(++seed);
				initial.fpcr = control != 0U ? test::fpcr_nep : 0U;
				const std::array values{
					0x3ff0'0000'0000'0000ULL};
				test::fill_source(initial, form, registers, values);
				char name[160];
				std::snprintf(name, sizeof(name),
					"floating-round-odd-allocation scalar=%u "
					"upper=%u nep=%u rd=%u rn=%u",
					static_cast<unsigned>(form.scalar),
					static_cast<unsigned>(form.upper), control,
					registers.destination, registers.source);
				test::check(harness, name, form, registers, initial);
			}
		}
	}
	harness.print_summary("arm64-simd-fcvtxn-allocation");
}

} // namespace

int main()
{
	every_form_register_edge_alias_and_scalar_nep();
	return 0;
}
