// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fmov_test_support.hpp"

namespace test = arm64_simd_fmov_test;
namespace base = arm64_simd_test;

namespace {

void register_moves_are_bit_exact_alias_safe_and_zero_upper()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U}) {
		const std::uint64_t mask = bytes == 8U ? ~std::uint64_t{0} :
			(std::uint64_t{1} << (bytes * 8U)) - 1U;
		for (const auto value : {0ULL, mask, 0x8000'0000'0000'0045ULL,
		     0x7fff'ffff'ffff'ff91ULL}) {
			auto initial = base::state(static_cast<unsigned>(value));
			initial.fpsr = 0x0800'0090U;
			test::write_bits(initial.vectors[29].bytes, 0U, bytes, value);
			test::require_scalar_result(
				test::register_instruction(bytes, 28U, 29U), bytes, 28U,
				value & mask, initial, harness);

			auto alias = initial;
			test::write_bits(alias.vectors[3].bytes, 0U, bytes, value);
			test::require_scalar_result(
				test::register_instruction(bytes, 3U, 3U), bytes, 3U,
				value & mask, alias, harness);
		}
	}
}

void all_immediate_encodings_expand_exactly()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U})
		for (unsigned immediate = 0; immediate < 256U; ++immediate) {
			auto initial = base::state(immediate + bytes);
			initial.fpsr = 0x0800'0080U;
			test::require_scalar_result(test::immediate_instruction(bytes,
				static_cast<std::uint8_t>(immediate), 28U), bytes, 28U,
				test::expand_immediate(static_cast<std::uint8_t>(immediate),
					bytes * 8U), initial, harness);
		}
}

} // namespace

int main()
{
	register_moves_are_bit_exact_alias_safe_and_zero_upper();
	all_immediate_encodings_expand_exactly();
	return 0;
}
