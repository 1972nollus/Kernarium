// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcsel_test_support.hpp"

namespace test = arm64_simd_fcsel_test;
namespace base = arm64_simd_test;
namespace move = arm64_simd_fmov_test;

namespace {

void every_nzcv_result_for_every_condition()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U})
		for (unsigned condition = 0; condition < 16U; ++condition)
			for (unsigned flags = 0; flags < 16U; ++flags) {
				auto initial = base::state(bytes * 31U + condition + flags);
				initial.nzcv = flags << 28;
				initial.fpsr = 0x0800'0090U;
				move::write_bits(initial.vectors[29].bytes, 0U, bytes,
					0x0123'4567'89ab'cdefULL);
				move::write_bits(initial.vectors[30].bytes, 0U, bytes,
					0xfedc'ba98'7654'3210ULL);
				test::require_result(test::instruction(bytes, condition,
					28U, 29U, 30U), bytes, 28U, 29U, 30U,
					initial, harness);
			}
}

void destination_aliases_either_source_safely()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U})
		for (const unsigned destination : {29U, 30U})
			for (const unsigned condition : {0U, 1U, 14U, 15U}) {
				auto initial = base::state(bytes + destination + condition);
				initial.nzcv = 0x4000'0000U;
				initial.fpsr = 0x80U;
				test::require_result(test::instruction(bytes, condition,
					destination, 29U, 30U), bytes, destination,
					29U, 30U, initial, harness);
			}
}

} // namespace

int main()
{
	every_nzcv_result_for_every_condition();
	destination_aliases_either_source_safely();
	return 0;
}
