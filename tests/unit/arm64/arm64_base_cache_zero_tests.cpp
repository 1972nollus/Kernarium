// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_cache_zero_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdlib>
#include <span>

namespace test = arm64_base_cache_zero_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

void production_specializes_source()
{
	constexpr auto pc = test::engine_tests::engine_test_code_base;
	const auto reg = gadgets::Builder{}.build(
		pc, std::array{test::instruction(0), 0xd400'0001U});
	const auto zero = gadgets::Builder{}.build(
		pc, std::array{test::instruction(31), 0xd400'0001U});
	if (!reg.complete || !zero.complete ||
	    reg.program.words().front() == zero.program.words().front())
		std::abort();
}

} // namespace

int main()
{
	production_specializes_source();
	return 0;
}
