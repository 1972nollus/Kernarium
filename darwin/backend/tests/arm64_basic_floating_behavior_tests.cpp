// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_basic_floating_test_support.hpp"

namespace test = arm64_basic_floating_test;

namespace {

template<typename Floating>
void check_form(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	constexpr test::Registers registers{0, 1, 2};
	auto initial = test::common::state(seed);
	initial.fpsr = 0;
	test::prepare_exact<Floating>(initial, form, registers);
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	const unsigned lanes = form.scalar ? 1U :
		form.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; ++lane)
		test::common::require(test::lane_bits<Floating>(actual,
			registers.destination, lane) ==
			std::bit_cast<test::FloatingInteger<Floating>>(
				test::exact_result<Floating>(form.operation, lane)));
	for (unsigned byte = form.scalar ? sizeof(Floating) : form.vector_bytes;
	     byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			std::byte{});
	test::common::require(actual.fpsr == 0U);
}

void exact_results_and_inactive_zeroing()
{
	test::common::Harness harness;
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.width == 16U)
			check_form<_Float16>(harness, form, ++seed);
		else if (form.width == 32U)
			check_form<float>(harness, form, ++seed);
		else
			check_form<double>(harness, form, ++seed);
	});
}

} // namespace

int main()
{
	exact_results_and_inactive_zeroing();
}
