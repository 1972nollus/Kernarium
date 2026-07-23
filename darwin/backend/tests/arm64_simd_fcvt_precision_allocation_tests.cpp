// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvt_precision_test_support.hpp"

#include <array>

namespace test = arm64_simd_fcvt_precision_test;
namespace base = arm64_simd_test;

namespace {

struct Pair final {
	unsigned source;
	unsigned destination;
};

constexpr std::array pairs{
	Pair{16U, 32U}, Pair{16U, 64U}, Pair{32U, 16U},
	Pair{32U, 64U}, Pair{64U, 16U}, Pair{64U, 32U},
};

void every_precision_pair_and_nep_mode()
{
	base::Harness harness;
	for (const auto pair : pairs)
		for (const bool merge : {false, true}) {
			auto initial = base::state(pair.source + pair.destination + merge);
			initial.fpcr = merge ? 4U : 0U;
			initial.fpsr = 0x0800'0090U;
			test::set_source(initial, 29U, pair.source, test::one(pair.source));
			test::require_result(test::instruction(pair.source,
				pair.destination, 28U, 29U), pair.destination, 28U,
				test::one(pair.destination), 0U, initial, harness);
		}
}

void destination_can_alias_source()
{
	base::Harness harness;
	for (const auto pair : pairs) {
		auto initial = base::state(pair.source * 3U + pair.destination);
		initial.fpsr = 0x10U;
		test::set_source(initial, 29U, pair.source, test::one(pair.source));
		test::require_result(test::instruction(pair.source, pair.destination,
			29U, 29U), pair.destination, 29U, test::one(pair.destination),
			0U, initial, harness);
	}
}

void every_reserved_type_pair_is_early_illegal()
{
	base::Harness harness;
	for (unsigned source_type = 0; source_type < 4U; ++source_type)
		for (unsigned destination_type = 0; destination_type < 4U;
		     ++destination_type) {
			/* ftype=01/opcode type=10 is not a reserved scalar FCVT pair:
			 * FEAT_BF16 assigns the complete allocation to BFCVT. */
			if (source_type == 1U && destination_type == 2U)
				continue;
			const bool valid = source_type != 2U && destination_type != 2U &&
				source_type != destination_type;
			if (valid)
				continue;
			const auto encoded = 0x1e22'4000U | (source_type << 22) |
				(destination_type << 15) | (29U << 5) | 28U;
			harness.require_illegal(encoded,
				base::state(source_type * 4U + destination_type));
		}
}

} // namespace

int main()
{
	every_precision_pair_and_nep_mode();
	destination_can_alias_source();
	every_reserved_type_pair_is_early_illegal();
	return 0;
}
