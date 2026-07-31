// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_mte_multiple_test_support.hpp"

namespace test = arm64_base_mte_multiple_test;
namespace feature_test = arm64_base_feature_system_test;
namespace engine = uml::backend::tests;

namespace {

void encoding_examples()
{
	feature_test::require(
		test::instruction(test::ldgm_base, 0, 0) == 0xd9e0'0000U);
	feature_test::require(
		test::instruction(test::ldgm_base, 31, 31) == 0xd9e0'03ffU);
}

} // namespace

int main()
{
	encoding_examples();
	engine::EngineHarness harness;
	test::check_register_matrix(
		harness, "BASE-0213", "LDGM", test::ldgm_base);
	feature_test::require_counts(harness, test::register_cases);
	harness.print_summary("arm64-base-mte-multiple-load");
}
