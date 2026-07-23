// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_complex_test_support.hpp"

namespace test = arm64_floating_complex_test;

namespace {

template<typename Floating>
void check_indexed(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	constexpr test::Registers registers{0, 1, 2};
	auto initial = test::common::state(seed);
	const unsigned lanes = form.vector_bytes / sizeof(Floating);
	const unsigned source_lanes = 16U / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; lane += 2U) {
		test::basic::set_lane(initial, registers.destination, lane,
			static_cast<Floating>(10));
		test::basic::set_lane(initial, registers.destination, lane + 1U,
			static_cast<Floating>(20));
		test::basic::set_lane(initial, registers.first, lane,
			static_cast<Floating>(2));
		test::basic::set_lane(initial, registers.first, lane + 1U,
			static_cast<Floating>(5));
	}
	for (unsigned lane = 0; lane < source_lanes; lane += 2U) {
		const auto complex = lane / 2U;
		test::basic::set_lane(initial, registers.second, lane,
			static_cast<Floating>(3U + complex * 2U));
		test::basic::set_lane(initial, registers.second, lane + 1U,
			static_cast<Floating>(4U + complex * 2U));
	}
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	const auto second_real = static_cast<Floating>(3U + form.index * 2U);
	const auto second_imaginary = static_cast<Floating>(4U + form.index * 2U);
	Floating real;
	Floating imaginary;
	test::expected_multiply_pair(form.rotation, static_cast<Floating>(10),
		static_cast<Floating>(20), static_cast<Floating>(2),
		static_cast<Floating>(5), second_real, second_imaginary,
		real, imaginary);
	for (unsigned lane = 0; lane < lanes; lane += 2U) {
		test::common::require(test::basic::lane_bits<Floating>(actual,
			registers.destination, lane) ==
			std::bit_cast<test::basic::FloatingInteger<Floating>>(real));
		test::common::require(test::basic::lane_bits<Floating>(actual,
			registers.destination, lane + 1U) ==
			std::bit_cast<test::basic::FloatingInteger<Floating>>(imaginary));
	}
	for (unsigned byte = form.vector_bytes; byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			std::byte{});
}

} // namespace

int main()
{
	test::common::Harness harness;
	unsigned seed = 200;
	test::for_each_form([&](const test::Form &form) {
		if (!form.by_element)
			return;
		if (form.width == 16U)
			check_indexed<_Float16>(harness, form, ++seed);
		else
			check_indexed<float>(harness, form, ++seed);
	});
}
