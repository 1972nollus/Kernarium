// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_horizontal_test_support.hpp"

namespace test = arm64_floating_horizontal_test;

namespace {

template<typename Floating>
void check_pairwise(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	constexpr test::Registers registers{0, 1, 2};
	auto initial = test::common::state(seed);
	const unsigned source_lanes = form.kind == test::Kind::scalar_pairwise ?
		2U : form.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < source_lanes; ++lane) {
		test::basic::set_lane(initial, registers.first, lane,
			static_cast<Floating>(lane + 1.25));
		test::basic::set_lane(initial, registers.second, lane,
			static_cast<Floating>(lane + 9.25));
	}
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	const unsigned result_lanes = form.kind == test::Kind::scalar_pairwise ?
		1U : source_lanes;
	for (unsigned lane = 0; lane < result_lanes; ++lane) {
		const bool second_source = form.kind == test::Kind::vector_pairwise &&
			lane >= source_lanes / 2U;
		const unsigned pair = form.kind == test::Kind::scalar_pairwise ? 0U :
			(lane % (source_lanes / 2U)) * 2U;
		const Floating base = static_cast<Floating>(
			(second_source ? 9.25 : 1.25) + pair);
		const auto expected = test::finite_result(form.operation, base,
			static_cast<Floating>(base + static_cast<Floating>(1)));
		test::common::require(test::basic::lane_bits<Floating>(actual,
			registers.destination, lane) ==
			std::bit_cast<test::basic::FloatingInteger<Floating>>(expected));
	}
	for (unsigned byte = form.kind == test::Kind::scalar_pairwise ?
		sizeof(Floating) : form.vector_bytes; byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			std::byte{});
}

void every_pairwise_shape_and_operation()
{
	test::common::Harness harness;
	unsigned seed = 100;
	test::for_each_form([&](const test::Form &form) {
		if (form.kind != test::Kind::scalar_pairwise &&
		    form.kind != test::Kind::vector_pairwise)
			return;
		if (form.width == 16U)
			check_pairwise<_Float16>(harness, form, ++seed);
		else if (form.width == 32U)
			check_pairwise<float>(harness, form, ++seed);
		else
			check_pairwise<double>(harness, form, ++seed);
	});
}

} // namespace

int main()
{
	every_pairwise_shape_and_operation();
}
