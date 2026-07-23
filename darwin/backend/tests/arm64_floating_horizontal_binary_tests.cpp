// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_horizontal_test_support.hpp"

namespace test = arm64_floating_horizontal_test;

namespace {

template<typename Floating>
void check_binary(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	constexpr test::Registers registers{0, 1, 2};
	auto initial = test::common::state(seed);
	const unsigned lanes = form.kind == test::Kind::scalar_binary ? 1U :
		form.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		test::basic::set_lane(initial, registers.first, lane,
			static_cast<Floating>(lane + 1.25));
		test::basic::set_lane(initial, registers.second, lane,
			static_cast<Floating>(8.5 - lane));
	}
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto expected = test::finite_result(form.operation,
			static_cast<Floating>(lane + 1.25),
			static_cast<Floating>(8.5 - lane));
		const auto expected_bits =
			std::bit_cast<test::basic::FloatingInteger<Floating>>(expected);
		if (const auto observed = test::basic::lane_bits<Floating>(actual,
			registers.destination, lane); observed != expected_bits) {
			std::fprintf(stderr,
				"horizontal binary mismatch insn=%08x op=%u kind=%u "
				"width=%u bytes=%u lane=%u expected=%llx actual=%llx\n",
				test::instruction(form, registers),
				static_cast<unsigned>(form.operation),
				static_cast<unsigned>(form.kind), form.width,
				form.vector_bytes, lane,
				static_cast<unsigned long long>(expected_bits),
				static_cast<unsigned long long>(observed));
			test::common::require(false);
		}
	}
	for (unsigned byte = form.kind == test::Kind::scalar_binary ?
		sizeof(Floating) : form.vector_bytes; byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			std::byte{});
}

void all_binary_results_and_inactive_lanes()
{
	test::common::Harness harness;
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.kind != test::Kind::scalar_binary &&
		    form.kind != test::Kind::vector_binary)
			return;
		if (form.width == 16U)
			check_binary<_Float16>(harness, form, ++seed);
		else if (form.width == 32U)
			check_binary<float>(harness, form, ++seed);
		else
			check_binary<double>(harness, form, ++seed);
	});
}

} // namespace

int main()
{
	all_binary_results_and_inactive_lanes();
}
