// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_integer_to_floating_test_support.hpp"

namespace test = arm64_integer_to_floating_test;

namespace {

[[nodiscard]] constexpr std::uint64_t allocation_input(
	const test::Form &form, unsigned seed) noexcept
{
	return (0x8123'4567'89ab'cdefULL ^
		(static_cast<std::uint64_t>(seed) *
			0x0101'0101'0101'0101ULL)) &
		test::low_mask(form.integer_width);
}

void every_fractional_immediate(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	unsigned forms{};
	test::for_each_form([&](const test::Form &form) {
		++forms;
		auto initial = test::state(++seed);
		test::fill_source(initial, form, {7U, 11U},
			allocation_input(form, seed));
		test::check(harness,
			"integer-to-floating-every-encoding-variant",
			form, {7U, 11U}, initial);
	});
	if (forms != 1156U)
		__builtin_trap();
}

void every_register_field(test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 10'000U;
	unsigned forms{};
	test::for_each_form([&](const test::Form &form) {
		++forms;
		for (unsigned destination = 0U; destination < 32U;
		     ++destination)
			for (unsigned source = 0U; source < 32U; ++source) {
				const test::Registers registers{destination, source};
				auto initial = test::state(++seed);
				test::fill_source(initial, form, registers,
					allocation_input(form, seed));
				test::check(harness,
					"integer-to-floating-every-register-field",
					form, registers, initial);
			}
	}, false);
	if (forms != 64U)
		__builtin_trap();
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_fractional_immediate(harness);
	every_register_field(harness);
	harness.print_summary("arm64-integer-to-floating-allocation");
	return 0;
}
