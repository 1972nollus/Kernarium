// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fabs_fneg_test_support.hpp"

namespace test = arm64_simd_fabs_fneg_test;
namespace base = arm64_simd_test;
namespace fp = arm64_simd_fp_indexed_test;

namespace {

void mixed_lanes_ah_and_short_vector_zeroing(
	test::engine_tests::EngineHarness &harness)
{
	for (std::size_t shape_index = 3U; shape_index < test::shapes.size();
	     ++shape_index) {
		const auto shape = test::shapes[shape_index];
		const auto sign = std::uint64_t{1} << (shape.lane_bytes * 8U - 1U);
		const auto infinity = shape.lane_bytes == 2U ? 0x7c00ULL :
			shape.lane_bytes == 4U ? 0x7f80'0000ULL :
				0x7ff0'0000'0000'0000ULL;
		const std::array values{0ULL, sign,
			fp::floating_bits(shape.lane_bytes, -1.5), infinity | sign,
			infinity | sign | 0x45U, infinity | 0x23U};
		for (const auto operation : {test::Operation::absolute,
		     test::Operation::negate})
			for (const bool alternative_handling : {false, true}) {
				auto initial = base::state(113U + static_cast<unsigned>(shape_index));
				initial.fpcr = alternative_handling ? 2U : 0U;
				initial.fpsr = 0x0800'0090U;
				const unsigned lanes = (shape.q ? 16U : 8U) /
					shape.lane_bytes;
				for (unsigned lane = 0; lane < lanes; ++lane)
					fp::write_bits(initial.vectors[29].bytes,
						shape.lane_bytes, lane, values[lane % values.size()]);
				test::check_result(harness, "fabs-fneg-vector-lanes",
					test::instruction(operation, shape, 28U, 29U),
					shape, operation, 28U, 29U, initial);
			}
	}
}

void source_destination_aliasing(test::engine_tests::EngineHarness &harness)
{
	for (std::size_t shape_index = 3U; shape_index < test::shapes.size();
	     ++shape_index) {
		const auto shape = test::shapes[shape_index];
		for (const auto operation : {test::Operation::absolute,
		     test::Operation::negate}) {
			auto initial = base::state(151U + static_cast<unsigned>(shape_index));
			initial.fpcr = 2U;
			initial.fpsr = 0x80U;
			fp::fill_bits(initial.vectors[7].bytes, shape.lane_bytes,
				fp::floating_bits(shape.lane_bytes, -4.0));
			test::check_result(harness, "fabs-fneg-vector-alias",
				test::instruction(operation, shape, 7U, 7U),
				shape, operation, 7U, 7U, initial);
		}
}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	mixed_lanes_ah_and_short_vector_zeroing(harness);
	source_destination_aliasing(harness);
	harness.print_summary("arm64-simd-fabs-fneg-vector");
	return 0;
}
