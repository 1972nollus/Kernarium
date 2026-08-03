// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_gcs_test_support.hpp"

#include <array>

namespace gcs = arm64_base_gcs_test;
namespace test = uml::emulator::tests;

namespace {

void encoding_examples()
{
	gcs::require(gcs::encode(
		gcs::Operation::pop_exception_context, 31) == 0xd508'77bfU);
	gcs::require(gcs::encode(gcs::Operation::pop_main, 0) == 0xd52b'7720U);
	gcs::require(gcs::encode(gcs::Operation::pop_exception, 31) ==
		0xd508'77dfU);
	gcs::require(gcs::encode(gcs::Operation::push_main, 0) == 0xd50b'7700U);
	gcs::require(gcs::encode(gcs::Operation::push_exception, 31) ==
		0xd508'779fU);
	gcs::require(gcs::encode(gcs::Operation::switch_stack_1, 0) ==
		0xd50b'7740U);
	gcs::require(gcs::encode(gcs::Operation::switch_stack_2, 31) ==
		0xd52b'777fU);
	gcs::require(gcs::encode(gcs::Operation::store, 5, 6) ==
		0xd91f'0cc5U);
	gcs::require(gcs::encode(gcs::Operation::store_unprivileged, 31, 31) ==
		0xd91f'1fffU);
}

void all_accessible_register_fields(test::EngineHarness &harness,
	std::size_t &cases)
{
	constexpr std::array system_operations{
		gcs::Operation::pop_main,
		gcs::Operation::push_main,
		gcs::Operation::switch_stack_1,
		gcs::Operation::switch_stack_2,
	};
	auto initial = gcs::initial_state();
	initial.gcs_implemented = false;
	for (const auto operation : system_operations) {
		for (unsigned transfer = 0; transfer < 32; ++transfer) {
			const gcs::CaseName name{"%s gcs-allocation-op%u-t%u",
				gcs::evidence_name(operation),
				static_cast<unsigned>(operation), transfer};
			gcs::check_allocated_illegal(harness, name,
				gcs::encode(operation, transfer), initial);
			++cases;
		}
	}

	for (const auto operation :
	     {gcs::Operation::store, gcs::Operation::store_unprivileged}) {
		for (unsigned base = 0; base < 32; ++base) {
			for (unsigned transfer = 0; transfer < 32; ++transfer) {
				const gcs::CaseName name{"%s gcs-allocation-op%u-b%u-t%u",
					gcs::evidence_name(operation),
					static_cast<unsigned>(operation), base, transfer};
				gcs::check_allocated_illegal(harness, name,
					gcs::encode(operation, transfer, base),
					initial);
				++cases;
			}
		}
	}
}

} // namespace

int main()
{
	encoding_examples();
	test::EngineHarness harness;
	std::size_t cases = 0;
	all_accessible_register_fields(harness, cases);
	gcs::require(cases == 2176);
	gcs::require_counts(harness, cases);
	harness.print_summary("arm64-base-gcs-allocation");
}
