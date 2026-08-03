// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_exception_test_support.hpp"

namespace {

void breakpoints()
{
	arm64_exception_test::Harness harness;
	for (unsigned immediate = 0; immediate <= 0xffffU; ++immediate)
		harness.check("base-breakpoint",
			arm64_exception_test::with_immediate(
				0xd420'0000U, immediate),
			uml::emulator::tests::ExpectedEvent::breakpoint, false);
	harness.print_summary("arm64-base-breakpoints");
}

} // namespace

int main()
{
	breakpoints();
	return 0;
}
