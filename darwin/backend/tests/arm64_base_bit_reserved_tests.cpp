// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_bit_test_support.hpp"

#include <cstdio>

namespace test = base_bit_test;
namespace engine_tests = uml::backend::tests;

int main()
{
	engine_tests::EngineHarness harness;
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 32U; ++operation) {
			if (test::allocated(wide, operation))
				continue;
			for (unsigned source = 0; source < 32U; ++source)
				for (unsigned destination = 0; destination < 32U;
				     ++destination) {
					auto initial = test::state(
						0xdead'beef'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							 wide)
						 << 55U) ^
						(static_cast<std::uint64_t>(
							 operation)
						 << 32U) ^
						(static_cast<std::uint64_t>(
							 source)
						 << 16U) ^
						destination);
					char name[112];
					std::snprintf(name, sizeof(name),
						"base-bit-reserved width=%u op=%u rn=%u rd=%u",
						wide ? 64U : 32U, operation,
						source, destination);
					harness.check_illegal(name,
						test::instruction(wide,
							operation, source,
							destination),
						initial);
				}
		}
	harness.print_summary("arm64-base-bit-reserved");
	return 0;
}
