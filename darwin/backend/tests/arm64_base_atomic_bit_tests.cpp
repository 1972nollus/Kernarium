// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_rmw_engine_test_support.hpp"

#include <array>

namespace atomic = arm64_atomic_rmw_engine_test;
namespace engine = uml::backend::tests;

int main()
{
	constexpr std::array operations{0U, 1U, 2U, 3U};
	engine::EngineHarness harness;
	for (const auto operation : operations)
		atomic::run_operation(harness, operation);
	harness.print_summary("arm64-base-atomic-bit");
}
