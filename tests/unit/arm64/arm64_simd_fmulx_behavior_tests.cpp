// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fmulx_test_support.hpp"

namespace test = arm64_simd_fmulx_test;
namespace fp = arm64_simd_fp_indexed_test;
namespace base = arm64_simd_test;

namespace {

[[nodiscard]] std::uint64_t sign_mask(unsigned lane_bytes)
{
	return std::uint64_t{1} << (lane_bytes * 8U - 1U);
}

[[nodiscard]] std::uint64_t infinity_bits(unsigned lane_bytes)
{
	if (lane_bytes == 2U)
		return 0x7c00U;
	if (lane_bytes == 4U)
		return 0x7f80'0000U;
	return 0x7ff0'0000'0000'0000ULL;
}

[[nodiscard]] std::uint64_t quiet_nan_bits(unsigned lane_bytes)
{
	if (lane_bytes == 2U)
		return 0x7e55U;
	if (lane_bytes == 4U)
		return 0x7fc0'0055U;
	return 0x7ff8'0000'0000'0055ULL;
}

[[nodiscard]] std::uint64_t signaling_nan_bits(unsigned lane_bytes)
{
	if (lane_bytes == 2U)
		return 0x7c55U;
	if (lane_bytes == 4U)
		return 0x7f80'0055U;
	return 0x7ff0'0000'0000'0055ULL;
}

[[nodiscard]] std::uint64_t default_nan_bits(unsigned lane_bytes)
{
	if (lane_bytes == 2U)
		return 0x7e00U;
	if (lane_bytes == 4U)
		return 0x7fc0'0000U;
	return 0x7ff8'0000'0000'0000ULL;
}

[[nodiscard]] std::uint64_t quiet_bit(unsigned lane_bytes)
{
	return std::uint64_t{1} <<
		(lane_bytes == 2U ? 9U : lane_bytes == 4U ? 22U : 51U);
}

void ordinary_and_aliasing_results(test::engine_tests::EngineHarness &harness)
{
	for (const auto shape : fp::shapes) {
		auto initial = base::state(0U);
		initial.fpsr = 0x80U;
		fp::fill_bits(initial.vectors[29].bytes, shape.lane_bytes,
			fp::floating_bits(shape.lane_bytes, 2.0));
		fp::fill_bits(initial.vectors[30].bytes, shape.lane_bytes,
			fp::floating_bits(shape.lane_bytes, 3.0));
		test::check_uniform(harness, "fmulx-ordinary", shape, 28U, 29U,
			30U, initial, fp::floating_bits(shape.lane_bytes, 6.0),
			0x80U);

		auto alias = base::state(0U);
		alias.fpsr = 0x80U;
		fp::fill_bits(alias.vectors[3].bytes, shape.lane_bytes,
			fp::floating_bits(shape.lane_bytes, 2.0));
		test::check_uniform(harness, "fmulx-register-alias", shape, 3U, 3U,
			3U, alias, fp::floating_bits(shape.lane_bytes, 4.0), 0x80U);
	}
}

void zero_infinity_and_nep_semantics(
	test::engine_tests::EngineHarness &harness)
{
	for (const auto shape : fp::shapes)
		for (const bool negative : {false, true}) {
			auto initial = base::state(0U);
			initial.fpsr = 0x80U;
			fp::fill_bits(initial.vectors[29].bytes, shape.lane_bytes,
				negative ? sign_mask(shape.lane_bytes) : 0U);
			fp::fill_bits(initial.vectors[30].bytes, shape.lane_bytes,
				infinity_bits(shape.lane_bytes));
			const auto result = fp::floating_bits(shape.lane_bytes, 2.0) |
				(negative ? sign_mask(shape.lane_bytes) : 0U);
			test::check_uniform(harness, "fmulx-zero-infinity", shape,
				28U, 29U, 30U, initial, result, 0x80U);
		}
	for (const auto shape : {fp::shapes[0], fp::shapes[3], fp::shapes[6]}) {
		auto initial = base::state(23U);
		initial.fpcr = 4U;
		initial.fpsr = 0;
		fp::write_bits(initial.vectors[29].bytes, shape.lane_bytes, 0U,
			fp::floating_bits(shape.lane_bytes, 2.0));
		fp::fill_bits(initial.vectors[30].bytes, shape.lane_bytes,
			fp::floating_bits(shape.lane_bytes, 3.0));
		test::check_uniform(harness, "fmulx-scalar-merging", shape,
			28U, 29U, 30U, initial,
			fp::floating_bits(shape.lane_bytes, 6.0), 0U);
	}
}

void overflow_and_rounding_modes(test::engine_tests::EngineHarness &harness)
{
	const auto shape = fp::shapes[5];
	constexpr std::array rounding_cases{
		std::array<std::uint32_t, 2>{0U, 0x7f80'0000U},
		std::array<std::uint32_t, 2>{1U << 22, 0x7f80'0000U},
		std::array<std::uint32_t, 2>{2U << 22, 0x7f7f'ffffU},
		std::array<std::uint32_t, 2>{3U << 22, 0x7f7f'ffffU},
	};
	for (const auto values : rounding_cases) {
		auto initial = base::state(values[0]);
		initial.fpcr = values[0];
		initial.fpsr = 0x80U;
		fp::fill_bits(initial.vectors[29].bytes, 4U, 0x7f7f'ffffU);
		fp::fill_bits(initial.vectors[30].bytes, 4U, 0x4000'0000U);
		test::check_uniform(harness, "fmulx-overflow-rounding", shape,
			28U, 29U, 30U, initial, values[1], 0x94U);
	}
}

void nan_processing(test::engine_tests::EngineHarness &harness)
{
	for (const auto shape : fp::shapes) {
		const auto one = fp::floating_bits(shape.lane_bytes, 1.0);
		const auto quiet = quiet_nan_bits(shape.lane_bytes);
		const auto signaling = signaling_nan_bits(shape.lane_bytes);

		auto quiet_initial = base::state(131U + shape.size);
		quiet_initial.fpsr = 0x80U;
		fp::fill_bits(quiet_initial.vectors[29].bytes, shape.lane_bytes,
			quiet);
		fp::fill_bits(quiet_initial.vectors[30].bytes, shape.lane_bytes, one);
		test::check_uniform(harness, "fmulx-quiet-nan", shape,
			28U, 29U, 30U, quiet_initial, quiet, 0x80U);

		auto signaling_initial = base::state(149U + shape.size);
		signaling_initial.fpsr = 0x80U;
		fp::fill_bits(signaling_initial.vectors[29].bytes, shape.lane_bytes,
			signaling);
		fp::fill_bits(signaling_initial.vectors[30].bytes,
			shape.lane_bytes, one);
		test::check_uniform(harness, "fmulx-signaling-nan", shape,
			28U, 29U, 30U, signaling_initial,
			signaling | quiet_bit(shape.lane_bytes), 0x81U);

		auto default_initial = quiet_initial;
		default_initial.fpcr = 1U << 25;
		default_initial.fpsr = 0x80U;
		test::check_uniform(harness, "fmulx-default-nan", shape,
			28U, 29U, 30U, default_initial,
			default_nan_bits(shape.lane_bytes), 0x80U);
	}
}

void subnormal_processing(test::engine_tests::EngineHarness &harness)
{
	for (const auto shape : fp::shapes) {
		const auto two = fp::floating_bits(shape.lane_bytes, 2.0);
		const auto half = fp::floating_bits(shape.lane_bytes, 0.5);

		auto exact = base::state(173U + shape.size);
		exact.fpsr = 0x80U;
		fp::fill_bits(exact.vectors[29].bytes, shape.lane_bytes, 1U);
		fp::fill_bits(exact.vectors[30].bytes, shape.lane_bytes, two);
		test::check_uniform(harness, "fmulx-exact-subnormal", shape,
			28U, 29U, 30U, exact, 2U, 0x80U);

		auto underflow = base::state(191U + shape.size);
		underflow.fpsr = 0x80U;
		fp::fill_bits(underflow.vectors[29].bytes, shape.lane_bytes, 1U);
		fp::fill_bits(underflow.vectors[30].bytes, shape.lane_bytes, half);
		test::check_uniform(harness, "fmulx-underflow", shape,
			28U, 29U, 30U, underflow, 0U, 0x98U);

		auto flushed = base::state(211U + shape.size);
		flushed.fpcr = shape.lane_bytes == 2U ? 1U << 19 : 1U << 24;
		flushed.fpsr = 0U;
		fp::fill_bits(flushed.vectors[29].bytes, shape.lane_bytes, 1U);
		fp::fill_bits(flushed.vectors[30].bytes, shape.lane_bytes, two);
		test::check_uniform(harness, "fmulx-flushed-input", shape,
			28U, 29U, 30U, flushed, 0U,
			shape.lane_bytes == 2U ? 0U : 0x80U);
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	ordinary_and_aliasing_results(harness);
	zero_infinity_and_nep_semantics(harness);
	overflow_and_rounding_modes(harness);
	nan_processing(harness);
	subnormal_processing(harness);
	harness.print_summary("arm64-simd-fmulx-behavior");
	return 0;
}
