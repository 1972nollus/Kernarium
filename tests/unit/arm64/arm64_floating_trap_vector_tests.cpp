// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_trap_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_trap_test;

namespace {

void every_arrangement(test::engine::EngineHarness &harness)
{
	struct Form final {
		const char *name;
		std::uint32_t instruction;
		unsigned destination;
		unsigned first;
		unsigned second;
		unsigned lane_bytes;
		unsigned lanes;
		std::uint64_t one;
		test::engine::NativeRequirements native;
	};
	const std::array forms{
		Form{"4h", 0x2e42'3c20U, 0U, 1U, 2U, 2U, 4U, 0x3c00U,
			test::vector_half_native()},
		Form{"8h", 0x6e45'3c83U, 3U, 4U, 5U, 2U, 8U, 0x3c00U,
			test::vector_half_native()},
		Form{"2s", 0x2e28'fce6U, 6U, 7U, 8U, 4U, 2U,
			0x3f80'0000U, test::vector_native()},
		Form{"4s", 0x6e2b'fd49U, 9U, 10U, 11U, 4U, 4U,
			0x3f80'0000U, test::vector_native()},
		Form{"2d", 0x6e6e'fdacU, 12U, 13U, 14U, 8U, 2U,
			0x3ff0'0000'0000'0000ULL, test::vector_native()},
	};
	std::uint64_t seed = 100U;
	for (const auto &form : forms) {
		auto initial = test::state(seed++);
		initial.fpcr = test::enable_for(test::fpsr_divide_by_zero);
		initial.fpsr |= test::fpsr_underflow;
		test::fill_lanes(initial, form.first, form.lane_bytes,
			form.lanes, form.one);
		test::fill_lanes(initial, form.second, form.lane_bytes,
			form.lanes, 0U);
		char name[96];
		std::snprintf(name, sizeof(name),
			"vector enabled divide-by-zero arrangement=%s",
			form.name);
		test::check_trap(harness, name, form.instruction, initial,
			test::fpsr_divide_by_zero, 0U, form.native);
	}
}

void lane_causes_are_collected_before_trap(
	test::engine::EngineHarness &harness)
{
	/* FDIV V9.4S, V10.4S, V11.4S.  Different lanes raise invalid,
	 * divide-by-zero, and inexact; an exact fourth lane is included too.
	 */
	constexpr std::uint32_t instruction = 0x6e2b'fd49U;
	auto initial = test::state(120U);
	constexpr auto causes = test::fpsr_invalid |
		test::fpsr_divide_by_zero | test::fpsr_inexact;
	initial.fpcr = test::enable_for(causes);
	const std::array first{
		0x0000'0000U, 0x3f80'0000U,
		0x3f80'0000U, 0x4000'0000U,
	};
	const std::array second{
		0x0000'0000U, 0x0000'0000U,
		0x4040'0000U, 0x3f80'0000U,
	};
	for (unsigned lane = 0; lane < first.size(); ++lane) {
		test::write_lane(initial, 10U, lane, 4U, first[lane]);
		test::write_lane(initial, 11U, lane, 4U, second[lane]);
	}
	test::check_trap(harness, "vector collects all lane causes",
		instruction, initial, causes, 0U, test::vector_native());
}

void aliasing_destination_rolls_back(test::engine::EngineHarness &harness)
{
	/* FDIV V10.4S, V10.4S, V11.4S exercises destination/source aliasing.
	 * The precise trap must retain every byte of the aliased input register.
	 */
	constexpr std::uint32_t instruction = 0x6e2b'fd4aU;
	auto initial = test::state(121U);
	initial.fpcr = test::enable_for(test::fpsr_divide_by_zero);
	test::fill_lanes(initial, 10U, 4U, 4U, 0x3f80'0000U);
	test::fill_lanes(initial, 11U, 4U, 4U, 0U);
	test::check_trap(harness, "vector aliased destination rollback",
		instruction, initial, test::fpsr_divide_by_zero, 0U,
		test::vector_native());
}

} // namespace

int main()
{
	test::engine::EngineHarness harness;
	every_arrangement(harness);
	lane_causes_are_collected_before_trap(harness);
	aliasing_destination_rolls_back(harness);
	harness.print_summary("arm64-floating-trap-vector");
	return 0;
}
