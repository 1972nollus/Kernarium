// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_matrix_test_support.hpp"

namespace test = arm64_half_matrix_test;

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts{};
	const auto initial = test::state(900U);
	struct Invalid final {
		std::uint32_t instruction;
		bool widening;
	};
	const std::array invalid{
		Invalid{0x0ec2'ec20U, false},
		Invalid{0x2ec2'ec20U, false},
		Invalid{0x6ec2'ec20U, false},
		Invalid{0x0e45'ec83U, true},
	};
	for (const auto reserved : invalid) {
		const auto native =
			test::native_requirements(reserved.widening);
		harness.check_illegal("half-matrix-reserved",
			reserved.instruction, initial, native);
		counts.add(native, true);
	}
	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-matrix-reserved");
}
