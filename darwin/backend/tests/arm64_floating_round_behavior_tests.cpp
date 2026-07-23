// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_round_test_support.hpp"

namespace test = arm64_floating_round_test;

namespace {

template<typename Floating>
void check_form(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	constexpr test::Registers registers{0, 1};
	auto initial = test::common::state(seed);
	initial.fpsr = 0;
	test::prepare<Floating>(initial, form, registers);
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	const auto expected = std::bit_cast<test::basic::FloatingInteger<Floating>>(
		test::expected_value<Floating>(form.operation));
	const unsigned lanes = form.scalar ? 1U :
		form.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; ++lane)
		if (const auto observed = test::basic::lane_bits<Floating>(actual,
			registers.destination, lane); observed != expected) {
			std::fprintf(stderr,
				"FRINT mismatch: insn=%08x op=%u scalar=%u width=%u bytes=%u "
				"lane=%u expected=%llx actual=%llx\n",
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

void rounding_results_and_inactive_zeroing()
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
	rounding_results_and_inactive_zeroing();
}
