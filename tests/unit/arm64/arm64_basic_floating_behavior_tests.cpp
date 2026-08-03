// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_basic_floating_test_support.hpp"

#include <array>

namespace test = arm64_basic_floating_test;

namespace {

struct LaneCase final {
	std::uint64_t left;
	std::uint64_t right;
	std::uint64_t result;
};

[[nodiscard]] LaneCase lane_case(const test::FormatBits &format,
	test::Operation operation, bool alternate)
{
	switch (operation) {
	case test::Operation::multiply:
		return alternate ?
			LaneCase{format.two, format.two_and_half, format.five} :
			LaneCase{format.one_and_half, format.two, format.three};
	case test::Operation::divide:
		return alternate ?
			LaneCase{format.five, format.two_and_half, format.two} :
			LaneCase{format.seven_and_half, format.two_and_half,
				format.three};
	case test::Operation::add:
		return alternate ?
			LaneCase{format.two, format.two, format.four} :
			LaneCase{format.one_and_half, format.two,
				format.three_and_half};
	case test::Operation::subtract:
		return alternate ?
			LaneCase{format.five, format.two, format.three} :
			LaneCase{format.five_and_half, format.two_and_quarter,
				format.three_and_quarter};
	case test::Operation::negative_multiply:
		return alternate ?
			LaneCase{format.two, format.two_and_half,
				test::negate_bits(format, format.five)} :
			LaneCase{format.one_and_half, format.two,
				test::negate_bits(format, format.three)};
	case test::Operation::square_root:
		return alternate ?
			LaneCase{format.four, 0, format.two} :
			LaneCase{format.nine, 0, format.three};
	}
	__builtin_unreachable();
}

void every_operation_precision_and_lane()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		constexpr test::Registers registers{0, 1, 2};
		auto initial = test::state(++seed);
		initial.fpsr = (1U << 27) | test::fpsr_input_denormal;
		const auto &format = test::format_bits(form.width);
		std::array<std::uint64_t, 8> results{};
		for (unsigned lane = 0; lane < test::active_lanes(form); ++lane) {
			const auto values =
				lane_case(format, form.operation, (lane & 1U) != 0U);
			test::set_lane_bits(initial, registers.left, lane,
				form.width, values.left);
			if (form.operation != test::Operation::square_root)
				test::set_lane_bits(initial, registers.right, lane,
					form.width, values.right);
			results[lane] = values.result;
		}
		harness.check_one("basic-floating-lane-behavior",
			test::instruction(form, registers), initial,
			test::expected_with_lane_results(initial, form, registers,
				std::span<const std::uint64_t>{
					results.data(), test::active_lanes(form)}),
			test::native_requirements(form));
	});
	harness.print_summary("arm64-basic-floating-behavior");
}

void vector_upper_half_is_zeroed()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 1000;
	for (const auto operation : test::vector_operations)
		for (const unsigned width : {16U, 32U}) {
			const test::Form form{operation, false, width, 8U};
			constexpr test::Registers registers{0, 1, 2};
			auto initial = test::state(++seed);
			initial.fpcr = 4U;
			initial.fpsr = 0;
			test::prepare_exact(initial, form, registers);
			const auto expected =
				test::expected_exact(initial, form, registers);
			for (unsigned byte = 8; byte < 16; ++byte)
				test::common::require(
					expected.vectors[registers.destination]
						.bytes[byte] == std::byte{});
			harness.check_one("basic-floating-vector-inactive-zero",
				test::instruction(form, registers), initial, expected,
				test::native_requirements(form));
		}
	harness.print_summary("arm64-basic-floating-inactive-zero");
}

} // namespace

int main()
{
	every_operation_precision_and_lane();
	vector_upper_half_is_zeroed();
	return 0;
}
