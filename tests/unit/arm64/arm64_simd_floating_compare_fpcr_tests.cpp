// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_floating_compare_test_support.hpp"

namespace test = arm64_simd_floating_compare_test;
namespace base = arm64_simd_test;

namespace {

void afp_subnormal_matrix(test::engine_tests::EngineHarness &harness)
{
	struct Entry final {
		std::uint32_t fpcr;
	};
	const Entry entries[]{
		{0U}, {1U << 24}, {1U}, {(1U << 24) | 1U},
		{2U}, {(1U << 24) | 2U}, {3U},
	};
	for (const auto shape : test::shapes) {
		if (shape.width == 16U)
			continue;
		for (const auto entry : entries) {
			auto initial = base::state(shape.width + entry.fpcr);
			initial.fpcr = entry.fpcr;
			initial.fpsr = 0x10U;
			const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
			for (unsigned lane = 0; lane < lanes; ++lane)
				test::write_lane(initial.vectors[29].bytes, shape, lane, 1U);
			test::check_result(harness,
				"simd-floating-compare-afp-subnormal", shape,
				test::Operation::equal, true, 28U, 29U, 0U,
				initial);
			test::write_lane(initial.vectors[30].bytes, shape, 0U, 0U);
			for (const auto operation :
			     {test::Operation::absolute_greater_equal,
			      test::Operation::absolute_greater})
				test::check_result(harness,
					"simd-floating-compare-absolute-afp-subnormal",
					shape, operation, false, 28U, 29U, 30U,
					initial);
		}
	}
}

void half_fz16_and_every_rare_operation(
	test::engine_tests::EngineHarness &harness)
{
	for (const auto shape : test::shapes) {
		for (const auto operation : test::zero_operations) {
			auto initial = base::state(shape.width + static_cast<unsigned>(operation));
			initial.fpcr = 3U;
			const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
			for (unsigned lane = 0; lane < lanes; ++lane) {
				test::write_lane(initial.vectors[29].bytes, shape, lane,
					test::positive_one(shape.width / 8U));
				test::write_lane(initial.vectors[30].bytes, shape, lane, 0U);
			}
			if (operation == test::Operation::less_equal ||
			    operation == test::Operation::less)
				test::check_result(harness,
					"simd-floating-compare-zero-control", shape,
					operation, true, 28U, 29U, 0U, initial);
			else {
				test::check_result(harness,
					"simd-floating-compare-register-control",
					shape, operation, false, 28U, 29U, 30U,
					initial);
			}
		}
	}

	for (const auto shape : {test::shapes[0], test::shapes[3], test::shapes[4]}) {
		auto initial = base::state(shape.vector_bytes);
		initial.fpcr = 1U << 19;
		const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
		for (unsigned lane = 0; lane < lanes; ++lane) {
			test::write_lane(initial.vectors[29].bytes, shape, lane, 1U);
		}
		test::check_result(harness,
			"simd-floating-compare-half-flush", shape,
			test::Operation::equal, true, 28U, 29U, 0U, initial);
		test::write_lane(initial.vectors[30].bytes, shape, 0U, 0U);
		for (const auto operation :
		     {test::Operation::absolute_greater_equal,
		      test::Operation::absolute_greater})
			test::check_result(harness,
				"simd-floating-compare-absolute-half-flush",
				shape, operation, false, 28U, 29U, 30U,
				initial);
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	afp_subnormal_matrix(harness);
	half_fz16_and_every_rare_operation(harness);
	harness.print_summary("arm64-simd-floating-compare-fpcr");
	return 0;
}
