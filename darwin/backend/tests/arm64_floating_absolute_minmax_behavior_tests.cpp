// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_absolute_minmax_test_support.hpp"

namespace test = arm64_floating_absolute_minmax_test;

namespace {

template<typename Floating>
void check_form(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	using Integer = test::basic::FloatingInteger<Floating>;
	constexpr test::Registers registers{0, 1, 2};
	auto initial = test::common::state(seed);
	initial.fpsr = 1U << 4;
	const unsigned lanes = form.vector_bytes / sizeof(Floating);
	constexpr unsigned bits = sizeof(Floating) * 8U;
	constexpr unsigned fraction_bits = sizeof(Floating) == 2 ? 10U :
		sizeof(Floating) == 4 ? 23U : 52U;
	constexpr Integer sign = Integer{1} << (bits - 1U);
	constexpr Integer one = static_cast<Integer>(
		((Integer{1} << (bits - fraction_bits - 1U)) - 1U) <<
		fraction_bits);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		Integer first;
		Integer second;
		switch (lane & 3U) {
		case 0: first = static_cast<Integer>(sign | (one + one / 2U));
			second = one; break;
		case 1: first = one;
			second = static_cast<Integer>(sign | (one + one / 4U)); break;
		case 2: first = sign; second = 0U; break;
		default: first = 1U; second = 2U; break;
		}
		test::basic::set_lane(initial, registers.first, lane,
			std::bit_cast<Floating>(first));
		test::basic::set_lane(initial, registers.second, lane,
			std::bit_cast<Floating>(second));
	}
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto first = test::basic::lane_bits<Floating>(initial,
			registers.first, lane);
		const auto second = test::basic::lane_bits<Floating>(initial,
			registers.second, lane);
		const auto expected = test::detail::floating_absolute_minmax(
			first, second,
			form.operation == test::Operation::absolute_maximum, false);
		test::common::require(test::basic::lane_bits<Floating>(actual,
			registers.destination, lane) == expected.bits);
	}
	for (unsigned byte = form.vector_bytes; byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			std::byte{});
	test::common::require(actual.fpsr == initial.fpsr);
}

} // namespace

int main()
{
	test::common::Harness harness;
	unsigned seed = 20;
	test::for_each_form([&](const test::Form &form) {
		if (form.width == 16U)
			check_form<_Float16>(harness, form, ++seed);
		else if (form.width == 32U)
			check_form<float>(harness, form, ++seed);
		else
			check_form<double>(harness, form, ++seed);
	});
}
