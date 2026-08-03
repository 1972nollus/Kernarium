// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_basic_floating_test_support.hpp"

namespace test = arm64_basic_floating_test;

namespace {

void every_destination_source_alias()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 500;
	test::for_each_form([&](const test::Form &form) {
		for (const auto registers : test::register_cases) {
			auto initial = test::state(++seed);
			initial.fpsr = seed & 1U ? 1U << 27 : 0U;
			test::prepare_exact(initial, form, registers);
			harness.check_one("basic-floating-register-alias",
				test::instruction(form, registers), initial,
				test::expected_exact(initial, form, registers),
				test::native_requirements(form));
		}
	});
	harness.print_summary("arm64-basic-floating-alias");
}

} // namespace

int main()
{
	every_destination_source_alias();
	return 0;
}
