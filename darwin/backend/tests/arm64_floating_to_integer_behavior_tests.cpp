// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_to_integer_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_to_integer_test;

namespace {

void every_nonfixed_variant_and_floating_class(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	unsigned forms{};
	test::for_each_nonfixed_form([&](const test::Form &form) {
		++forms;
		const auto inputs = test::representative_inputs(form.source_width);
		for (unsigned index = 0U; index < inputs.size(); ++index) {
			const test::Registers registers{7U, 12U};
			auto initial = test::state(++seed);
			std::array<std::uint64_t, 8> lanes{};
			for (unsigned lane = 0U; lane < lanes.size(); ++lane)
				lanes[lane] = inputs[(index + lane * 3U) % inputs.size()];
			test::fill_source(initial, form, registers, lanes);
			char name[192];
			std::snprintf(name, sizeof(name),
				"floating-to-integer-behavior destination=%u rounding=%u "
				"unsigned=%u scalar=%u source=%u integer=%u bytes=%u "
				"class=%u",
				static_cast<unsigned>(form.destination),
				static_cast<unsigned>(form.rounding),
				static_cast<unsigned>(form.unsigned_result),
				static_cast<unsigned>(form.scalar), form.source_width,
				form.integer_width, form.vector_bytes, index);
			test::check(harness, name, form, registers, initial);
		}
	});
	if (forms != 180U)
		__builtin_trap();
}

void finite_range_boundaries(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 10'000U;
	test::for_each_nonfixed_form([&](const test::Form &form) {
		const int boundary = form.unsigned_result ?
			static_cast<int>(form.integer_width) :
			static_cast<int>(form.integer_width - 1U);
		const std::array inputs{
			test::power_of_two(form.source_width, boundary - 1),
			test::power_of_two(form.source_width, boundary),
			test::power_of_two(form.source_width, boundary + 1),
			test::power_of_two(form.source_width, boundary, true),
			test::power_of_two(form.source_width, boundary + 1, true)};
		for (unsigned index = 0U; index < inputs.size(); ++index) {
			const test::Registers registers{18U, 19U};
			auto initial = test::state(++seed);
			const std::array selected{inputs[index]};
			test::fill_source(initial, form, registers, selected);
			char name[160];
			std::snprintf(name, sizeof(name),
				"floating-to-integer-range destination=%u rounding=%u "
				"unsigned=%u source=%u integer=%u boundary=%u",
				static_cast<unsigned>(form.destination),
				static_cast<unsigned>(form.rounding),
				static_cast<unsigned>(form.unsigned_result),
				form.source_width, form.integer_width, index);
			test::check(harness, name, form, registers, initial);
		}
	});
}

void lane_order_zeroing_and_destructive_aliases(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 20'000U;
	test::for_each_nonfixed_form([&](const test::Form &form) {
		if (form.destination == test::Destination::gpr)
			return;
		const test::Registers registers{15U, 15U};
		auto initial = test::state(++seed);
		const auto inputs = test::representative_inputs(form.source_width);
		test::fill_source(initial, form, registers, inputs);
		test::check(harness,
			"floating-to-integer-lane-order-zeroing-and-alias",
			form, registers, initial);
	});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_nonfixed_variant_and_floating_class(harness);
	finite_range_boundaries(harness);
	lane_order_zeroing_and_destructive_aliases(harness);
	harness.print_summary("arm64-floating-to-integer-behavior");
	return 0;
}
