// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void nan_is_invalid_for_every_rounding_and_destination_kind()
{
	base::Harness harness;
	for (const auto rounding : test::roundings)
		for (const bool unsigned_result : {false, true}) {
			auto gpr = base::state(static_cast<unsigned>(rounding));
			test::set_gpr_source(gpr, 29U, 32U,
				test::precision::quiet_nan(32U));
			test::require_gpr(test::gpr_instruction(rounding,
				unsigned_result, 32U, 64U, 28U, 29U), 64U, 28U,
				0U, 1U, gpr, harness);

			auto fpr = base::state(30U + static_cast<unsigned>(rounding));
			test::set_gpr_source(fpr, 29U, 16U,
				test::precision::quiet_nan(16U));
			test::require_fpr(test::fpr_instruction(rounding,
				unsigned_result, 16U, 32U, 28U, 29U), 32U, 28U,
				0U, 1U, fpr, harness);

			const auto shape = test::shapes[6];
			auto simd = base::state(60U + static_cast<unsigned>(rounding));
			std::array<std::uint64_t, 4> outputs{};
			for (unsigned lane = 0; lane < outputs.size(); ++lane)
				test::compare::write_lane(simd.vectors[29].bytes, shape,
					lane, test::precision::quiet_nan(32U));
			test::require_simd(test::simd_instruction(rounding,
				unsigned_result, shape, 28U, 29U), shape, 28U,
				outputs, 1U, simd, harness);
		}
}

} // namespace

int main()
{
	nan_is_invalid_for_every_rounding_and_destination_kind();
	return 0;
}
