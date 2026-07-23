// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_horizontal_test_support.hpp"

namespace test = arm64_floating_horizontal_test;

namespace {

template<typename Floating>
void check_reduction(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	constexpr test::Registers registers{0, 1, 2};
	auto initial = test::common::state(seed);
	const unsigned lanes = form.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; ++lane)
		test::basic::set_lane(initial, registers.first, lane,
			static_cast<Floating>(lane % 2U == 0U ? lane + 1.25 : 20.25 - lane));
	auto expected = static_cast<Floating>(1.25);
	for (unsigned lane = 1; lane < lanes; ++lane)
		expected = test::finite_result(form.operation, expected,
			static_cast<Floating>(lane % 2U == 0U ?
				lane + 1.25 : 20.25 - lane));
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	test::common::require(test::basic::lane_bits<Floating>(actual,
		registers.destination, 0) ==
		std::bit_cast<test::basic::FloatingInteger<Floating>>(expected));
	for (unsigned byte = sizeof(Floating); byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			std::byte{});
}

void every_reduction_shape_and_operation()
{
	test::common::Harness harness;
	unsigned seed = 300;
	test::for_each_form([&](const test::Form &form) {
		if (form.kind != test::Kind::reduction)
			return;
		if (form.width == 16U)
			check_reduction<_Float16>(harness, form, ++seed);
		else
			check_reduction<float>(harness, form, ++seed);
	});
}

} // namespace

int main()
{
	every_reduction_shape_and_operation();
}
