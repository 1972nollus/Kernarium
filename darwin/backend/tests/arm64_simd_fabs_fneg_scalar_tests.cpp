// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fabs_fneg_test_support.hpp"

namespace test = arm64_simd_fabs_fneg_test;
namespace base = arm64_simd_test;
namespace fp = arm64_simd_fp_indexed_test;

namespace {

[[nodiscard]] std::array<std::uint64_t, 10> edge_values(unsigned lane_bytes)
{
	const auto sign = std::uint64_t{1} << (lane_bytes * 8U - 1U);
	const auto infinity = lane_bytes == 2U ? 0x7c00ULL :
		lane_bytes == 4U ? 0x7f80'0000ULL : 0x7ff0'0000'0000'0000ULL;
	const auto quiet_nan = infinity | (std::uint64_t{1} <<
		(lane_bytes == 2U ? 9U : lane_bytes == 4U ? 22U : 51U)) | 0x25U;
	const auto signaling_nan = infinity | 0x19U;
	return {0U, sign, fp::floating_bits(lane_bytes, 1.5),
		fp::floating_bits(lane_bytes, -2.25), infinity, infinity | sign,
		quiet_nan, quiet_nan | sign, signaling_nan, signaling_nan | sign};
}

void ieee_edges_nan_payloads_and_ah(test::engine_tests::EngineHarness &harness)
{
	for (const auto shape : {test::shapes[0], test::shapes[1], test::shapes[2]})
		for (const auto operation : {test::Operation::absolute,
		     test::Operation::negate})
			for (const bool alternative_handling : {false, true})
				for (const auto value : edge_values(shape.lane_bytes)) {
					auto initial = base::state(static_cast<unsigned>(value));
					initial.fpcr = alternative_handling ? 2U : 0U;
					initial.fpsr = 0x0800'0090U;
					fp::write_bits(initial.vectors[29].bytes,
						shape.lane_bytes, 0U, value);
					test::check_result(harness, "fabs-fneg-scalar-edge",
						test::instruction(operation, shape, 28U, 29U),
						shape, operation, 28U, 29U, initial);
				}
}

void nep_merges_old_destination_and_aliases_safely(
	test::engine_tests::EngineHarness &harness)
{
	for (const auto shape : {test::shapes[0], test::shapes[1], test::shapes[2]})
		for (const auto operation : {test::Operation::absolute,
		     test::Operation::negate}) {
			auto initial = base::state(79U + shape.lane_bytes);
			initial.fpcr = 4U;
			initial.fpsr = 0x80U;
			fp::write_bits(initial.vectors[29].bytes, shape.lane_bytes, 0U,
				fp::floating_bits(shape.lane_bytes, -3.5));
			test::check_result(harness, "fabs-fneg-scalar-merging",
				test::instruction(operation, shape, 28U, 29U),
				shape, operation, 28U, 29U, initial);

			auto alias = base::state(91U + shape.lane_bytes);
			alias.fpcr = 6U;
			alias.fpsr = 0x0800'0080U;
			const auto sign = std::uint64_t{1} <<
				(shape.lane_bytes * 8U - 1U);
			const auto infinity = shape.lane_bytes == 2U ? 0x7c00ULL :
				shape.lane_bytes == 4U ? 0x7f80'0000ULL :
					0x7ff0'0000'0000'0000ULL;
			fp::write_bits(alias.vectors[3].bytes, shape.lane_bytes, 0U,
				infinity | sign | 0x55U);
			test::check_result(harness, "fabs-fneg-scalar-alias",
				test::instruction(operation, shape, 3U, 3U),
				shape, operation, 3U, 3U, alias);
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	ieee_edges_nan_payloads_and_ah(harness);
	nep_merges_old_destination_and_aliases_safely(harness);
	harness.print_summary("arm64-simd-fabs-fneg-scalar");
	return 0;
}
