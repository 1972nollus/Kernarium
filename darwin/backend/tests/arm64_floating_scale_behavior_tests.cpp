// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_scale_test_support.hpp"

#include <cmath>

namespace test = arm64_floating_scale_test;

namespace {

template<typename Floating, typename Integer>
void check_form(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	constexpr test::Registers registers{0, 1, 2};
	auto initial = test::common::state(seed);
	initial.fpsr = 0;
	const unsigned lanes = form.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const Floating value = static_cast<Floating>(
			(lane & 1U) == 0U ? 1.5 : -0.75);
		const auto scale = static_cast<std::make_signed_t<Integer>>(
			(lane & 1U) == 0U ? 3 : -2);
		test::basic::set_lane(initial, registers.first, lane, value);
		test::set_scale_lane<Floating>(initial, registers.second, lane,
			std::bit_cast<Integer>(scale));
	}
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const Floating expected = static_cast<Floating>(
			(lane & 1U) == 0U ? 12.0 : -0.1875);
		test::common::require(test::basic::lane_bits<Floating>(actual,
			registers.destination, lane) ==
			std::bit_cast<Integer>(expected));
	}
	for (unsigned byte = form.vector_bytes; byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			std::byte{});
	test::common::require(actual.fpsr == 0U);
}

} // namespace

int main()
{
	test::common::Harness harness;
	unsigned seed = 10;
	test::for_each_form([&](const test::Form &form) {
		if (form.width == 16U)
			check_form<_Float16, std::uint16_t>(harness, form, ++seed);
		else if (form.width == 32U)
			check_form<float, std::uint32_t>(harness, form, ++seed);
		else
			check_form<double, std::uint64_t>(harness, form, ++seed);
	});
}
