// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fmov_test_support.hpp"

namespace test = arm64_simd_fmov_test;
namespace base = arm64_simd_test;

namespace {

void register_moves_are_bit_exact_alias_safe_and_zero_upper(
	test::engine_tests::EngineHarness &harness)
{
	for (const unsigned bytes : {2U, 4U, 8U}) {
		const std::uint64_t mask = bytes == 8U ? ~std::uint64_t{0} :
			(std::uint64_t{1} << (bytes * 8U)) - 1U;
		for (const auto value : {0ULL, mask, 0x8000'0000'0000'0045ULL,
		     0x7fff'ffff'ffff'ff91ULL}) {
			auto initial = base::state(static_cast<unsigned>(value));
			initial.fpsr = 0x0800'0090U;
			test::write_bits(initial.vectors[29].bytes, 0U, bytes, value);
			test::check_scalar_result(harness, "fmov-register-bits",
				test::register_instruction(bytes, 28U, 29U), bytes,
				28U, value & mask, initial);

			auto alias = initial;
			test::write_bits(alias.vectors[3].bytes, 0U, bytes, value);
			test::check_scalar_result(harness, "fmov-register-alias",
				test::register_instruction(bytes, 3U, 3U), bytes,
				3U, value & mask, alias);
		}
	}
}

void all_immediate_encodings_expand_exactly(
	test::engine_tests::EngineHarness &harness)
{
	for (const unsigned bytes : {2U, 4U, 8U})
		for (unsigned immediate = 0; immediate < 256U; ++immediate) {
			auto initial = base::state(immediate + bytes);
			initial.fpsr = 0x0800'0080U;
			test::check_scalar_result(harness, "fmov-immediate-expansion",
				test::immediate_instruction(bytes,
					static_cast<std::uint8_t>(immediate), 28U),
				bytes, 28U,
				test::expand_immediate(static_cast<std::uint8_t>(immediate),
					bytes * 8U), initial);
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	register_moves_are_bit_exact_alias_safe_and_zero_upper(harness);
	all_immediate_encodings_expand_exactly(harness);
	harness.print_summary("arm64-simd-fmov-scalar");
	return 0;
}
