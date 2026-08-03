// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_gcs_test_support.hpp"

#include <array>

namespace gcs = arm64_base_gcs_test;
namespace test = uml::emulator::tests;

namespace {

void el0_exception_aliases(test::EngineHarness &harness,
	std::size_t &cases)
{
	constexpr std::array operations{
		gcs::Operation::pop_exception_context,
		gcs::Operation::pop_exception,
		gcs::Operation::push_exception,
	};
	const auto initial = gcs::initial_state();
	for (const auto operation : operations) {
		for (unsigned transfer = 0; transfer < 32; ++transfer) {
			const gcs::CaseName name{
				"%s gcs-el0-undefined-op%u-t%u",
				gcs::evidence_name(operation),
				static_cast<unsigned>(operation), transfer};
			/* Rt==31 is the named form. Other Rt values are constrained
			 * unpredictable and Undefined is one permitted behavior. The
			 * EL0 access check precedes all exception-state observation. */
			gcs::check_allocated_illegal(harness, name,
				gcs::encode(operation, transfer), initial);
			++cases;
		}
	}
}

void decoder_neighbors_are_not_gcs(test::EngineHarness &harness,
	std::size_t &cases)
{
	constexpr std::array operations{
		gcs::Operation::pop_exception_context,
		gcs::Operation::pop_main,
		gcs::Operation::pop_exception,
		gcs::Operation::push_main,
		gcs::Operation::push_exception,
		gcs::Operation::switch_stack_1,
		gcs::Operation::switch_stack_2,
	};
	const auto initial = gcs::initial_state();
	for (const auto operation : operations) {
		const auto instruction = gcs::encode(operation, 7) ^ 0x400U;
		const gcs::CaseName name{"gcs-decoder-neighbor-op%u",
			static_cast<unsigned>(operation)};
		harness.check({
			.name = name,
			.instruction = instruction,
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::illegal_instruction,
			.native = test::NativeRequirements::baseline(),
			.invalid_encoding = true,
		});
		++cases;
	}
}

} // namespace

int main()
{
	test::EngineHarness harness;
	std::size_t cases = 0;
	el0_exception_aliases(harness, cases);
	decoder_neighbors_are_not_gcs(harness, cases);
	gcs::require(cases == 103);
	gcs::require_counts(harness, cases);
	harness.print_summary("arm64-base-gcs-reserved");
}
