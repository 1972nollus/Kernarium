// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_integer_to_floating_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_integer_to_floating_test;

namespace {

[[nodiscard]] constexpr std::array<std::uint64_t, 7>
boundary_inputs(const test::Form &form) noexcept
{
	const auto mask = test::low_mask(form.integer_width);
	const auto sign = test::sign_mask(form.integer_width);
	const auto precision =
		test::format(form.destination_width).fraction_bits + 1U;
	const auto rounding_edge = precision + 1U < form.integer_width ?
		(std::uint64_t{1} << precision) + 1U :
		(mask / 3U) | 1U;
	return {
		0U,
		1U,
		rounding_edge,
		sign - 1U,
		sign,
		sign + 1U,
		mask,
	};
}

void every_boundary_for_every_variant(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	unsigned forms{};
	for (unsigned pass = 0U; pass < 2U; ++pass)
		test::for_each_form([&](const test::Form &form) {
			if (static_cast<unsigned>(form.unsigned_source) != pass)
				return;
			++forms;
			const auto inputs = boundary_inputs(form);
			for (unsigned index = 0U; index < inputs.size(); ++index) {
				const test::Registers registers{5U, 9U};
				auto initial = test::state(++seed);
				test::fill_source(initial, form, registers,
					inputs[index], true);
				char name[160];
				std::snprintf(name, sizeof(name),
					"integer-to-floating-boundary unsigned=%u "
					"class=%u iw=%u fw=%u bytes=%u fbits=%u input=%u",
					static_cast<unsigned>(form.unsigned_source),
					static_cast<unsigned>(form.encoding),
					form.integer_width, form.destination_width,
					form.vector_bytes, form.fractional_bits, index);
				test::check(harness, name, form, registers, initial);
			}
		});
	if (forms != 1156U)
		__builtin_trap();
}

void vector_lane_order_and_inactive_zeroing(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 20'000U;
	test::for_each_form([&](const test::Form &form) {
		if (form.scalar)
			return;
		const test::Registers registers{13U, 17U};
		auto initial = test::state(++seed);
		const auto mask = test::low_mask(form.integer_width);
		for (unsigned lane = 0U; lane < test::lanes(form); ++lane) {
			const auto value =
				(static_cast<std::uint64_t>(lane + 1U) *
					0x1111'2222'3333'4445ULL) & mask;
			test::write_lane(initial.vectors[registers.source].bytes,
				lane, form.integer_width, value);
		}
		test::check(harness,
			"integer-to-floating-vector-lane-order-and-zeroing",
			form, registers, initial);
	}, false);
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_boundary_for_every_variant(harness);
	vector_lane_order_and_inactive_zeroing(harness);
	harness.print_summary("arm64-integer-to-floating-behavior");
	return 0;
}
