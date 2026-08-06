// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_system_operation_test_support.hpp"

#include <cstddef>

namespace operations = arm64_base_system_operation_test;
namespace engine = uml::emulator::tests;
namespace feature_test = arm64_base_feature_system_test;
using ::uml::cpu::ArmFeature;

namespace {

void every_named_operation_and_pair(engine::EngineHarness &harness)
{
	const auto d128_native = engine::NativeRequirements::features(
		{ArmFeature::sysinstr128, ArmFeature::d128});
	const auto tlbid_native = engine::NativeRequirements::features(
		{ArmFeature::sysinstr128, ArmFeature::tlbid});
	for (const auto key : operations::tlbip_named) {
		const unsigned op1 = key >> 11;
		const unsigned crn = (key >> 7) & 15U;
		const unsigned crm = (key >> 3) & 15U;
		const unsigned op2 = key & 7U;
		feature_test::require(operations::named_tlbip_operation(
			op1, crn, crm, op2));
		for (unsigned first = 0; first < 32; ++first) {
			const auto instruction =
				operations::system_instruction_pair(
					op1, crn, crm, op2, first);
			if ((first & 1U) == 0U || first == 31U) {
				const feature_test::CaseName d128_name{
					"tlbip-d128-%04x-t%u", key, first};
				feature_test::check_profile_illegal(
					harness, d128_name, instruction,
					d128_native);
				if (operations::tlbip_supports_tlbid(
						op1, crn, crm, op2)) {
					/*
					 * These operation-table rows are allocated by
					 * FEAT_D128 or FEAT_TLBID. D128-only rows must not
					 * acquire a spurious TLBID native gate.
					 */
					const feature_test::CaseName tlbid_name{
						"tlbip-tlbid-%04x-t%u", key, first};
					feature_test::check_profile_illegal(
						harness, tlbid_name, instruction,
						tlbid_native);
				}
			} else {
				const feature_test::CaseName reserved_name{
					"tlbip-reserved-rt-%04x-t%u",
					key, first};
				harness.check_illegal(reserved_name, instruction,
					arm64_system_test::state());
			}
		}
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	every_named_operation_and_pair(harness);
	constexpr std::size_t tlbid_operations =
		operations::tlbip_named.size() -
		operations::tlbip_d128_only.size();
	constexpr std::size_t cases =
		operations::tlbip_named.size() * (17U + 15U) +
		tlbid_operations * 17U;
	operations::require_mandatory_jit(
		harness, cases, operations::tlbip_named.size() * 15U);
	harness.print_summary("arm64-base-tlbip-named");
}
