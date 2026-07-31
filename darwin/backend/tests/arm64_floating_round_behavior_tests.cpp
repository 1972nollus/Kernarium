// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_round_test_support.hpp"

#include <array>

namespace test = arm64_floating_round_test;

namespace {

struct InputCase final {
	std::string_view name;
	std::uint64_t (*bits)(const test::Format &);
};

[[nodiscard]] std::uint64_t positive_zero(const test::Format &format)
{
	return test::zero_bits(format);
}

[[nodiscard]] std::uint64_t negative_zero(const test::Format &format)
{
	return test::zero_bits(format, true);
}

[[nodiscard]] std::uint64_t positive_quarter(const test::Format &format)
{
	return test::power_of_two_bits(format, -2);
}

[[nodiscard]] std::uint64_t negative_quarter(const test::Format &format)
{
	return test::power_of_two_bits(format, -2, true);
}

[[nodiscard]] std::uint64_t positive_half(const test::Format &format)
{
	return test::power_of_two_bits(format, -1);
}

[[nodiscard]] std::uint64_t negative_half(const test::Format &format)
{
	return test::power_of_two_bits(format, -1, true);
}

[[nodiscard]] std::uint64_t positive_three_quarters(
	const test::Format &format)
{
	return test::dyadic_bits(format, 3U, -2);
}

[[nodiscard]] std::uint64_t negative_three_quarters(
	const test::Format &format)
{
	return test::dyadic_bits(format, 3U, -2, true);
}

[[nodiscard]] std::uint64_t positive_one_and_half(
	const test::Format &format)
{
	return test::dyadic_bits(format, 3U, -1);
}

[[nodiscard]] std::uint64_t negative_one_and_half(
	const test::Format &format)
{
	return test::dyadic_bits(format, 3U, -1, true);
}

[[nodiscard]] std::uint64_t positive_two_and_quarter(
	const test::Format &format)
{
	return test::dyadic_bits(format, 9U, -2);
}

[[nodiscard]] std::uint64_t negative_two_and_quarter(
	const test::Format &format)
{
	return test::dyadic_bits(format, 9U, -2, true);
}

[[nodiscard]] std::uint64_t positive_two_and_half(
	const test::Format &format)
{
	return test::dyadic_bits(format, 5U, -1);
}

[[nodiscard]] std::uint64_t negative_two_and_half(
	const test::Format &format)
{
	return test::dyadic_bits(format, 5U, -1, true);
}

[[nodiscard]] std::uint64_t positive_three_and_half(
	const test::Format &format)
{
	return test::dyadic_bits(format, 7U, -1);
}

[[nodiscard]] std::uint64_t negative_three_and_half(
	const test::Format &format)
{
	return test::dyadic_bits(format, 7U, -1, true);
}

[[nodiscard]] std::uint64_t positive_integral(const test::Format &format)
{
	return test::dyadic_bits(format, 9U, 0);
}

[[nodiscard]] std::uint64_t negative_integral(const test::Format &format)
{
	return test::dyadic_bits(format, 9U, 0, true);
}

[[nodiscard]] std::uint64_t positive_infinity(const test::Format &format)
{
	return test::infinity_bits(format);
}

[[nodiscard]] std::uint64_t negative_infinity(const test::Format &format)
{
	return test::infinity_bits(format, true);
}

[[nodiscard]] std::uint64_t quiet_nan(const test::Format &format)
{
	return test::quiet_nan_bits(format);
}

[[nodiscard]] std::uint64_t negative_quiet_nan(const test::Format &format)
{
	return test::quiet_nan_bits(format, true);
}

[[nodiscard]] std::uint64_t signaling_nan(const test::Format &format)
{
	return test::signaling_nan_bits(format);
}

[[nodiscard]] std::uint64_t negative_signaling_nan(
	const test::Format &format)
{
	return test::signaling_nan_bits(format, true);
}

[[nodiscard]] std::uint64_t minimum_subnormal(const test::Format &format)
{
	return test::minimum_subnormal_bits(format);
}

[[nodiscard]] std::uint64_t negative_minimum_subnormal(
	const test::Format &format)
{
	return test::minimum_subnormal_bits(format, true);
}

constexpr std::array input_cases{
	InputCase{"positive-zero", positive_zero},
	InputCase{"negative-zero", negative_zero},
	InputCase{"positive-quarter", positive_quarter},
	InputCase{"negative-quarter", negative_quarter},
	InputCase{"positive-half", positive_half},
	InputCase{"negative-half", negative_half},
	InputCase{"positive-three-quarters", positive_three_quarters},
	InputCase{"negative-three-quarters", negative_three_quarters},
	InputCase{"positive-one-and-half", positive_one_and_half},
	InputCase{"negative-one-and-half", negative_one_and_half},
	InputCase{"positive-two-and-quarter", positive_two_and_quarter},
	InputCase{"negative-two-and-quarter", negative_two_and_quarter},
	InputCase{"positive-two-and-half", positive_two_and_half},
	InputCase{"negative-two-and-half", negative_two_and_half},
	InputCase{"positive-three-and-half", positive_three_and_half},
	InputCase{"negative-three-and-half", negative_three_and_half},
	InputCase{"positive-integral", positive_integral},
	InputCase{"negative-integral", negative_integral},
	InputCase{"positive-infinity", positive_infinity},
	InputCase{"negative-infinity", negative_infinity},
	InputCase{"quiet-not-a-number", quiet_nan},
	InputCase{"negative-quiet-not-a-number", negative_quiet_nan},
	InputCase{"signaling-not-a-number", signaling_nan},
	InputCase{"negative-signaling-not-a-number", negative_signaling_nan},
	InputCase{"minimum-subnormal", minimum_subnormal},
	InputCase{"negative-minimum-subnormal", negative_minimum_subnormal},
};

void every_form_and_boundary_value(
	test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1};
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		const auto format = test::format(form.width);
		for (const auto &input : input_cases) {
			auto initial = test::state(++seed);
			test::fill_active_lanes(initial, registers.source, form,
				input.bits(format));
			harness.check_one(input.name,
				test::instruction(form, registers), initial,
				test::expected_state(initial, form, registers),
				test::native_requirements(form));
		}
	});
}

void mixed_vector_lanes_and_inactive_zeroing(
	test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1};
	unsigned seed = 10'000U;
	test::for_each_form([&](const test::Form &form) {
		if (form.scalar)
			return;
		auto initial = test::state(++seed);
		const auto format = test::format(form.width);
		const std::array values{
			test::dyadic_bits(format, 5U, -1),
			test::dyadic_bits(format, 9U, -2, true),
			test::signaling_nan_bits(format),
			test::minimum_subnormal_bits(format, true),
			test::zero_bits(format, true),
			test::infinity_bits(format),
			test::dyadic_bits(format, 7U, -1),
			test::quiet_nan_bits(format, true),
		};
		const unsigned lanes =
			form.vector_bytes / (form.width / 8U);
		for (unsigned lane = 0; lane < lanes; ++lane)
			test::write_lane(initial.vectors[registers.source].bytes,
				lane, form.width, values[lane]);
		harness.check_one("floating-round-mixed-vector-lanes",
			test::instruction(form, registers), initial,
			test::expected_state(initial, form, registers),
			test::native_requirements(form));
	});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_form_and_boundary_value(harness);
	mixed_vector_lanes_and_inactive_zeroing(harness);
	harness.print_summary("arm64-floating-round-behavior");
	return 0;
}
