// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_mte_multiple_test_support.hpp"

namespace test = arm64_base_mte_multiple_test;
namespace feature_test = arm64_base_feature_system_test;
namespace engine = uml::backend::tests;

namespace {

void encoding_examples()
{
	feature_test::require(
		test::instruction(test::stgm_base, 0, 0) == 0xd9a0'0000U);
	feature_test::require(
		test::instruction(test::stgm_base, 31, 31) == 0xd9a0'03ffU);
	feature_test::require(
		test::instruction(test::stzgm_base, 0, 0) == 0xd920'0000U);
	feature_test::require(
		test::instruction(test::stzgm_base, 31, 31) == 0xd920'03ffU);
}

} // namespace

int main()
{
	encoding_examples();
	engine::EngineHarness harness;
	test::check_register_matrix(
		harness, "BASE-0403", "STGM", test::stgm_base);
	test::check_register_matrix(
		harness, "BASE-0463", "STZGM", test::stzgm_base);
	feature_test::require_counts(harness, 2U * test::register_cases);
	harness.print_summary("arm64-base-mte-multiple-store");
}
