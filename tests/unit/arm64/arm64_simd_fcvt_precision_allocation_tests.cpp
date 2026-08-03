// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvt_precision_test_support.hpp"

#include <cstdio>

namespace test = arm64_simd_fcvt_precision_test;

namespace {

void every_precision_pair_register_field_alias_and_merge_mode(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	for (const auto pair : test::pairs)
		for (const bool merge : {false, true})
			for (unsigned destination = 0U; destination < 32U;
			     ++destination)
				for (unsigned source = 0U; source < 32U; ++source) {
					auto initial = test::state(++seed);
					initial.fpcr = merge ?
						test::oracle::fpcr_nep : 0U;
					initial.fpsr = 0x0800'0090U;
					test::set_source(initial, source,
						pair.source_width,
						test::one(pair.source_width));
					char name[192];
					std::snprintf(name, sizeof(name),
						"scalar-precision-allocation source=%u "
						"destination=%u merge=%u rd=%u rn=%u",
						pair.source_width,
						pair.destination_width,
						static_cast<unsigned>(merge),
						destination, source);
					test::check(harness, name, pair,
						{destination, source}, initial);
				}
}

void every_reserved_type_pair_and_register_field_is_illegal(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 0x10000U;
	for (unsigned source_type = 0U; source_type < 4U; ++source_type)
		for (unsigned destination_type = 0U;
		     destination_type < 4U; ++destination_type) {
			if (test::valid_type_pair(source_type, destination_type) ||
			    test::belongs_to_bfloat_conversion(source_type,
				    destination_type))
				continue;
			for (unsigned destination = 0U; destination < 32U;
			     ++destination)
				for (unsigned source = 0U; source < 32U; ++source) {
					auto initial = test::state(++seed);
					char name[192];
					std::snprintf(name, sizeof(name),
						"scalar-precision-reserved source-type=%u "
						"destination-type=%u rd=%u rn=%u",
						source_type, destination_type,
						destination, source);
					test::check_illegal(harness, name,
						source_type, destination_type,
						{destination, source}, initial);
				}
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_precision_pair_register_field_alias_and_merge_mode(harness);
	every_reserved_type_pair_and_register_field_is_illegal(harness);
	harness.print_summary("arm64-simd-fcvt-precision-allocation");
	return 0;
}
