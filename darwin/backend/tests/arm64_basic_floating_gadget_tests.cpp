// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_basic_floating_test_support.hpp"

namespace test = arm64_basic_floating_test;

namespace {

void every_specialized_gadget_and_overlap()
{
	test::common::Harness harness;
	unsigned seed = 500;
	test::for_each_form([&](const test::Form &form) {
		for (const auto registers : test::register_cases)
			harness.require_differential(test::instruction(form, registers),
				test::common::state(++seed));
	});
}

} // namespace

int main()
{
	every_specialized_gadget_and_overlap();
}
