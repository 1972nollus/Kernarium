// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_test_support.hpp"

namespace test = arm64_fp8_test;

namespace {

constexpr std::uint64_t fpmr_mask = 0x0000'003f'ff7f'c1ffULL;

void read_write_and_reserved_bits()
{
	test::common::Harness harness;
	auto initial = test::common::state(400);
	initial.registers[5] = ~std::uint64_t{};
	const auto written = harness.run_interpreter(0xd51b'4445U, initial);
	test::common::require(written.fpmr == fpmr_mask);
	harness.require_differential(0xd51b'4445U, initial);

	auto read_initial = initial;
	read_initial.fpmr = 0xffff'ffff'ffff'ffffULL;
	const auto read = harness.run_interpreter(0xd53b'4447U, read_initial);
	test::common::require(read.registers[7] == fpmr_mask);
	harness.require_differential(0xd53b'4447U, read_initial);
}

void zero_register_aliases()
{
	test::common::Harness harness;
	auto initial = test::common::state(401);
	initial.fpmr = test::fpmr(1, 1, 1, 17, 0x81, true, true, 33);
	const auto discarded = harness.run_interpreter(0xd53b'445fU, initial);
	test::common::require(discarded.fpmr == initial.fpmr);
	harness.require_differential(0xd53b'445fU, initial);
	const auto zeroed = harness.run_interpreter(0xd51b'445fU, initial);
	test::common::require(zeroed.fpmr == 0);
	harness.require_differential(0xd51b'445fU, initial);
}

void fpcr_extended_controls_are_writable()
{
	test::common::Harness harness;
	auto initial = test::common::state(402);
	initial.registers[3] = 7U;
	const auto actual = harness.run_interpreter(0xd51b'4403U, initial);
	test::common::require((actual.fpcr & 7U) == 7U);
	harness.require_differential(0xd51b'4403U, initial);
}

} // namespace

int main()
{
	read_write_and_reserved_bits();
	zero_register_aliases();
	fpcr_extended_controls_are_writable();
}
