// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_complex_test_support.hpp"

#include <cstdio>

namespace test = arm64_floating_complex_test;

namespace {

template<typename Floating>
void check_multiply(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	constexpr test::Registers registers{0, 1, 2};
	auto initial = test::common::state(seed);
	const unsigned lanes = form.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; lane += 2U) {
		test::basic::set_lane(initial, registers.destination, lane,
			static_cast<Floating>(10));
		test::basic::set_lane(initial, registers.destination, lane + 1U,
			static_cast<Floating>(20));
		test::basic::set_lane(initial, registers.first, lane,
			static_cast<Floating>(2));
		test::basic::set_lane(initial, registers.first, lane + 1U,
			static_cast<Floating>(5));
		test::basic::set_lane(initial, registers.second, lane,
			static_cast<Floating>(3));
		test::basic::set_lane(initial, registers.second, lane + 1U,
			static_cast<Floating>(4));
	}
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	Floating real;
	Floating imaginary;
	test::expected_multiply_pair(form.rotation, static_cast<Floating>(10),
		static_cast<Floating>(20), static_cast<Floating>(2),
		static_cast<Floating>(5), static_cast<Floating>(3),
		static_cast<Floating>(4), real, imaginary);
	for (unsigned lane = 0; lane < lanes; lane += 2U) {
		const auto actual_real = test::basic::lane_bits<Floating>(actual,
			registers.destination, lane);
		const auto expected_real =
			std::bit_cast<test::basic::FloatingInteger<Floating>>(real);
		if (actual_real != expected_real)
			std::fprintf(stderr,
				"FCMLA width=%u bytes=%u rotation=%u lane=%u actual=%llx expected=%llx\n",
				form.width, form.vector_bytes, form.rotation, lane,
				static_cast<unsigned long long>(actual_real),
				static_cast<unsigned long long>(expected_real));
		test::common::require(actual_real == expected_real);
		test::common::require(test::basic::lane_bits<Floating>(actual,
			registers.destination, lane + 1U) ==
			std::bit_cast<test::basic::FloatingInteger<Floating>>(imaginary));
	}
}

} // namespace

int main()
{
	test::common::Harness harness;
	unsigned seed = 100;
	test::for_each_form([&](const test::Form &form) {
		if (form.operation != test::Operation::multiply_accumulate ||
		    form.by_element)
			return;
		if (form.width == 16U)
			check_multiply<_Float16>(harness, form, ++seed);
		else if (form.width == 32U)
			check_multiply<float>(harness, form, ++seed);
		else
			check_multiply<double>(harness, form, ++seed);
	});
}
