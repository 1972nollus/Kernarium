// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_dot_test_support.hpp"

namespace test = arm64_half_dot_test;

namespace {

void every_unallocated_unsigned_vector_size_cell()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	unsigned seed = 16000U;
	for (const bool q : {false, true})
		for (const unsigned size : {0U, 2U}) {
			harness.check_illegal(
				"FDOT vector U=1 size is reserved",
				test::raw_vector_instruction(
					q, true, size, {31U, 30U, 29U}),
				test::state(++seed));
			counts.add(
				test::engine_tests::NativeRequirements::baseline(),
				true);
		}
	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-dot-reserved-vector");
}

void nonoverlapping_scalar_class_encodings_are_unallocated()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	unsigned seed = 17000U;
	for (const auto form : test::forms) {
		/*
		 * Setting bit 28 on an indexed narrow form enters the allocated
		 * scalar three-source class. Those exact encodings are covered by
		 * the fused-floating allocation suite, not by this reserved oracle.
		 */
		if (form.by_element && !form.wide)
			continue;
		for (const auto registers : test::register_aliases) {
			harness.check_illegal(
				"FDOT nonoverlapping scalar-class encoding is reserved",
				test::instruction(form, registers) |
					0x1000'0000U,
				test::state(++seed));
			counts.add(
				test::engine_tests::NativeRequirements::baseline(),
					true);
		}
	}
	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-dot-reserved-scalar");
}

} // namespace

int main()
{
	every_unallocated_unsigned_vector_size_cell();
	nonoverlapping_scalar_class_encodings_are_unallocated();
	return 0;
}
