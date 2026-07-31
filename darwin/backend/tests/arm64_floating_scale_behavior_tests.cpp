// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_scale_test_support.hpp"

#include <array>
#include <cstdio>
#include <limits>

namespace test = arm64_floating_scale_test;

namespace {

constexpr test::Registers registers{0, 1, 2};

[[nodiscard]] constexpr std::int64_t minimum_scale_value(
	unsigned width) noexcept
{
	return width == 64U ? std::numeric_limits<std::int64_t>::min() :
		-(std::int64_t{1} << (width - 1U));
}

[[nodiscard]] constexpr std::int64_t maximum_scale_value(
	unsigned width) noexcept
{
	return width == 64U ? std::numeric_limits<std::int64_t>::max() :
		(std::int64_t{1} << (width - 1U)) - 1;
}

void prepare_lanes(test::arm64::CpuState &initial, const test::Form &form,
	std::uint64_t primary, std::int64_t scale)
{
	const auto format = test::format(form.width);
	const unsigned lanes = form.vector_bytes / (form.width / 8U);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		std::uint64_t input = primary;
		switch (lane & 3U) {
		case 1:
			input ^= test::sign_mask(format);
			break;
		case 2:
			input = test::minimum_normal_bits(format);
			break;
		case 3:
			input = test::maximum_subnormal_bits(format, true);
			break;
		}
		test::write_lane(initial.vectors[registers.first].bytes,
			lane, form.width, input);
		test::write_lane(initial.vectors[registers.second].bytes,
			lane, form.width, test::scale_bits(scale, form.width));
	}
}

void check(test::engine_tests::EngineHarness &harness, const char *name,
	const test::Form &form, std::uint64_t input, std::int64_t scale,
	unsigned seed)
{
	auto initial = test::state(seed);
	prepare_lanes(initial, form, input, scale);
	harness.check_one(name, test::instruction(form, registers), initial,
		test::expected_state(initial, form, registers),
		test::native_requirements());
}

void every_distinct_clamped_scale(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	for (const auto &form : test::forms) {
		const auto format = test::format(form.width);
		const auto exponent = static_cast<std::int64_t>(format.bias);
		const auto low =
			-static_cast<std::int64_t>(format.fraction_bits + 1U) -
			exponent;
		const auto high =
			static_cast<std::int64_t>(test::maximum_exponent(format)) +
			static_cast<std::int64_t>(format.fraction_bits + 1U) -
			exponent;
		const auto input = test::normal_bits(format,
			static_cast<unsigned>(exponent),
			test::fraction_mask(format) / 3U);

		/*
		 * Every value in [low, high] is a distinct clamped result.
		 * The four outer values cover both adjacent saturation classes and
		 * both signed-operand endpoints; all other encodings are equivalent
		 * to one of these cases under FPClampScale.
		 */
		for (auto scale = low; scale <= high; ++scale)
			check(harness, "floating-scale-distinct-clamped-scale",
				form, input, scale, ++seed);
		for (const auto scale : std::array{
			     low - 1, high + 1, minimum_scale_value(form.width),
			     maximum_scale_value(form.width)})
			check(harness, "floating-scale-saturated-scale-class",
				form, input, scale, ++seed);
	}
}

void every_exponent_field_at_both_clamp_boundaries(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 10'000U;
	for (const auto &form : test::forms) {
		const auto format = test::format(form.width);
		for (unsigned exponent = 0;
		     exponent < test::maximum_exponent(format); ++exponent) {
			const auto input = exponent == 0U ?
				test::maximum_subnormal_bits(format) :
				test::normal_bits(format, exponent,
					test::fraction_mask(format) / 5U);
			const auto low =
				-static_cast<std::int64_t>(
					format.fraction_bits + 1U) -
				static_cast<std::int64_t>(exponent);
			const auto high =
				static_cast<std::int64_t>(
					test::maximum_exponent(format)) +
				static_cast<std::int64_t>(
					format.fraction_bits + 1U) -
				static_cast<std::int64_t>(exponent);
			check(harness, "floating-scale-low-clamp-by-exponent",
				form, input, low - 1, ++seed);
			check(harness, "floating-scale-high-clamp-by-exponent",
				form, input, high + 1, ++seed);
		}
	}
}

void every_input_class_and_sign(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 30'000U;
	for (const auto &form : test::forms) {
		const auto format = test::format(form.width);
		const std::array inputs{
			test::zero_bits(format),
			test::zero_bits(format, true),
			test::minimum_subnormal_bits(format),
			test::minimum_subnormal_bits(format, true),
			test::maximum_subnormal_bits(format),
			test::minimum_normal_bits(format),
			test::normal_bits(format,
				static_cast<unsigned>(format.bias),
				test::fraction_mask(format) / 3U),
			test::maximum_finite_bits(format),
			test::maximum_finite_bits(format, true),
			test::infinity_bits(format),
			test::infinity_bits(format, true),
			test::quiet_nan_bits(format),
			test::signaling_nan_bits(format),
			test::signaling_nan_bits(format, true),
		};
		for (unsigned input_index = 0; input_index < inputs.size();
		     ++input_index)
			for (const std::int64_t scale : {-1, 0, 1}) {
				char name[128];
				std::snprintf(name, sizeof(name),
					"floating-scale-input-class width=%u bytes=%u "
					"input=%u scale=%lld",
					form.width, form.vector_bytes, input_index,
					static_cast<long long>(scale));
				check(harness, name, form, inputs[input_index],
					scale, ++seed);
			}
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_distinct_clamped_scale(harness);
	every_exponent_field_at_both_clamp_boundaries(harness);
	every_input_class_and_sign(harness);
	harness.print_summary("arm64-floating-scale-behavior");
	return 0;
}
