// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_complex_test_support.hpp"

namespace test = arm64_floating_complex_test;

namespace {

template<typename Floating>
void check_add(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	constexpr test::Registers registers{0, 1, 2};
	auto initial = test::common::state(seed);
	const unsigned lanes = form.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; lane += 2U) {
		test::basic::set_lane(initial, registers.first, lane,
			static_cast<Floating>(1));
		test::basic::set_lane(initial, registers.first, lane + 1U,
			static_cast<Floating>(2));
		test::basic::set_lane(initial, registers.second, lane,
			static_cast<Floating>(3));
		test::basic::set_lane(initial, registers.second, lane + 1U,
			static_cast<Floating>(4));
	}
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	const Floating expected_real = static_cast<Floating>(
		form.rotation == 1U ? -3 : 5);
	const Floating expected_imaginary = static_cast<Floating>(
		form.rotation == 1U ? 5 : -1);
	for (unsigned lane = 0; lane < lanes; lane += 2U) {
		test::common::require(test::basic::lane_bits<Floating>(actual,
			registers.destination, lane) ==
			std::bit_cast<test::basic::FloatingInteger<Floating>>(expected_real));
		test::common::require(test::basic::lane_bits<Floating>(actual,
			registers.destination, lane + 1U) ==
			std::bit_cast<test::basic::FloatingInteger<Floating>>(
				expected_imaginary));
	}
	for (unsigned byte = form.vector_bytes; byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			std::byte{});
}

} // namespace

int main()
{
	test::common::Harness harness;
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.operation != test::Operation::add)
			return;
		if (form.width == 16U)
			check_add<_Float16>(harness, form, ++seed);
		else if (form.width == 32U)
			check_add<float>(harness, form, ++seed);
		else
			check_add<double>(harness, form, ++seed);
	});
}
