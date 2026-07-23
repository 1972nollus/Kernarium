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

void ordinary_and_aliasing_results()
{
	base::Harness harness;
	for (const auto shape : fp::shapes) {
		auto initial = base::state(0U);
		initial.fpsr = 0x80U;
		fp::fill_bits(initial.vectors[29].bytes, shape.lane_bytes,
			fp::floating_bits(shape.lane_bytes, 2.0));
		fp::fill_bits(initial.vectors[30].bytes, shape.lane_bytes,
			fp::floating_bits(shape.lane_bytes, 3.0));
		const auto encoded = test::instruction(shape, 28U, 29U, 30U);
		fp::require_uniform_result(encoded, shape, fp::Operation::fmulx,
			28U, 29U, initial, fp::floating_bits(shape.lane_bytes, 6.0),
			0x80U, harness);

		auto alias = base::state(0U);
		alias.fpsr = 0x80U;
		fp::fill_bits(alias.vectors[3].bytes, shape.lane_bytes,
			fp::floating_bits(shape.lane_bytes, 2.0));
		fp::require_uniform_result(test::instruction(shape, 3U, 3U, 3U),
			shape, fp::Operation::fmulx, 3U, 3U, alias,
			fp::floating_bits(shape.lane_bytes, 4.0), 0x80U, harness);
	}
}

void zero_infinity_and_nep_semantics()
{
	base::Harness harness;
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
			fp::require_uniform_result(test::instruction(shape, 28U, 29U, 30U),
				shape, fp::Operation::fmulx, 28U, 29U, initial, result,
				0x80U, harness);
		}
	for (const auto shape : {fp::shapes[0], fp::shapes[3], fp::shapes[6]}) {
		auto initial = base::state(23U);
		initial.fpcr = 4U;
		initial.fpsr = 0;
		fp::write_bits(initial.vectors[29].bytes, shape.lane_bytes, 0U,
			fp::floating_bits(shape.lane_bytes, 2.0));
		fp::fill_bits(initial.vectors[30].bytes, shape.lane_bytes,
			fp::floating_bits(shape.lane_bytes, 3.0));
		fp::require_uniform_result(test::instruction(shape, 28U, 29U, 30U),
			shape, fp::Operation::fmulx, 28U, 29U, initial,
			fp::floating_bits(shape.lane_bytes, 6.0), 0U, harness);
	}
}

void overflow_sets_cumulative_flags()
{
	base::Harness harness;
	const auto shape = fp::shapes[5];
	auto initial = base::state(0U);
	initial.fpsr = 0x80U;
	fp::fill_bits(initial.vectors[29].bytes, 4U, 0x7f7f'ffffU);
	fp::fill_bits(initial.vectors[30].bytes, 4U, 0x4000'0000U);
	const auto encoded = test::instruction(shape, 28U, 29U, 30U);
	const auto result = harness.run_interpreter(encoded, initial);
	base::require((result.fpsr & 0x94U) == 0x94U);
	harness.require_differential(encoded, initial);
}

} // namespace

int main()
{
	ordinary_and_aliasing_results();
	zero_infinity_and_nep_semantics();
	overflow_sets_cumulative_flags();
	return 0;
}
