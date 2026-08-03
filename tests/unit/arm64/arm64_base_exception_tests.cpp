// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_exception_test_support.hpp"

namespace {

void service_calls()
{
	arm64_exception_test::Harness harness;
	for (unsigned immediate = 0; immediate <= 0xffffU; ++immediate) {
		harness.check("base-service-call",
			arm64_exception_test::with_immediate(
				0xd400'0001U, immediate),
			uml::emulator::tests::ExpectedEvent::system_call, true);
	}
	harness.print_summary("arm64-base-service-calls");
}

} // namespace

int main()
{
	service_calls();
	return 0;
}
