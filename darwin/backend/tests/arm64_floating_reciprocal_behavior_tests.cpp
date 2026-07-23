// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_reciprocal_test_support.hpp"

namespace test = arm64_floating_reciprocal_test;

namespace {

template<typename Floating>
void check_form(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	constexpr test::Registers registers{0, 1, 2};
	auto initial = test::common::state(seed);
	const unsigned lanes = form.scalar ? 1U :
		form.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		test::basic::set_lane(initial, registers.first, lane,
			test::first_input<Floating>(form.operation));
		test::basic::set_lane(initial, registers.second, lane,
			test::second_input<Floating>(form.operation));
	}
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	const auto expected = test::expected_bits<Floating>(form.operation);
	for (unsigned lane = 0; lane < lanes; ++lane)
		if (const auto observed = test::basic::lane_bits<Floating>(actual,
			registers.destination, lane); observed != expected) {
			std::fprintf(stderr,
				"reciprocal mismatch insn=%08x op=%u scalar=%u width=%u "
				"bytes=%u lane=%u expected=%llx actual=%llx\n",
				test::instruction(form, registers),
				static_cast<unsigned>(form.operation), form.scalar,
				form.width, form.vector_bytes, lane,
				static_cast<unsigned long long>(expected),
				static_cast<unsigned long long>(observed));
			test::common::require(false);
		}
	for (unsigned byte = form.scalar ? sizeof(Floating) : form.vector_bytes;
	     byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			std::byte{});
}

} // namespace

int main()
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
