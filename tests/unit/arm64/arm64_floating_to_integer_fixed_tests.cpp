// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_to_integer_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_to_integer_test;

namespace {

void every_fixed_point_scale_and_variant(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	unsigned forms{};
	test::for_each_fixed_form([&](const test::Form &form) {
		++forms;
		const auto representatives =
			test::representative_inputs(form.source_width);
		const std::array inputs{
			test::power_of_two(form.source_width,
				-static_cast<int>(form.fractional_bits)),
			test::power_of_two(form.source_width,
				1 - static_cast<int>(form.fractional_bits)),
			representatives[14],
			representatives[15],
			representatives[16],
			representatives[18]};
		for (unsigned index = 0U; index < inputs.size(); ++index) {
			const test::Registers registers{9U, 10U};
			auto initial = test::state(++seed);
			std::array<std::uint64_t, 8> lane_inputs{};
			for (unsigned lane = 0U; lane < lane_inputs.size(); ++lane)
				lane_inputs[lane] =
					inputs[(index + lane) % inputs.size()];
			test::fill_source(initial, form, registers, lane_inputs);
			char name[192];
			std::snprintf(name, sizeof(name),
				"floating-to-integer-fixed destination=%u unsigned=%u "
				"scalar=%u source=%u integer=%u bytes=%u fraction=%u "
				"input=%u",
				static_cast<unsigned>(form.destination),
				static_cast<unsigned>(form.unsigned_result),
				static_cast<unsigned>(form.scalar), form.source_width,
				form.integer_width, form.vector_bytes,
				form.fractional_bits, index);
			test::check(harness, name, form, registers, initial);
		}
	});
	if (forms != 1'120U)
		__builtin_trap();
}

void fixed_point_scaling_range_edges(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 20'000U;
	test::for_each_fixed_form([&](const test::Form &form) {
		const int boundary = static_cast<int>(
			form.unsigned_result ? form.integer_width :
				form.integer_width - 1U) -
			static_cast<int>(form.fractional_bits);
		const std::array inputs{
			test::power_of_two(form.source_width, boundary - 1),
			test::power_of_two(form.source_width, boundary),
			test::power_of_two(form.source_width, boundary + 1),
			test::power_of_two(form.source_width, boundary, true)};
		const test::Registers registers{27U, 28U};
		auto initial = test::state(++seed);
		test::fill_source(initial, form, registers, inputs);
		test::check(harness, "floating-to-integer-fixed-range-edge",
			form, registers, initial);
	}, false);
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_fixed_point_scale_and_variant(harness);
	fixed_point_scaling_range_edges(harness);
	harness.print_summary("arm64-floating-to-integer-fixed");
	return 0;
}
