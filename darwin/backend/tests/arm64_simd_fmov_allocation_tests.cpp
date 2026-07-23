// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fmov_test_support.hpp"

namespace test = arm64_simd_fmov_test;
namespace base = arm64_simd_test;

namespace {

inline constexpr std::array general_encodings{
	0x1ee6'0020U, 0x9ee6'0062U, 0x1ee7'00a4U, 0x9ee7'00e6U,
	0x1e26'0128U, 0x1e27'016aU, 0x9e66'01acU, 0x9e67'01eeU,
	0x9eae'0230U, 0x9eaf'0272U};

void every_scalar_precision_and_general_variant()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U}) {
		auto initial = base::state(bytes);
		initial.fpsr = 0x0800'0090U;
		test::write_bits(initial.vectors[29].bytes, 0U, bytes,
			0xfedc'ba98'7654'3210ULL);
		test::require_scalar_result(test::register_instruction(bytes, 28U, 29U),
			bytes, 28U, test::read_bits(initial.vectors[29].bytes, 0U, bytes),
			initial, harness);
		test::require_scalar_result(test::immediate_instruction(bytes, 0xd5U,
			28U), bytes, 28U, test::expand_immediate(0xd5U, bytes * 8U),
			initial, harness);
	}
	for (const auto encoded : general_encodings)
		harness.require_differential(encoded, base::state(encoded));
}

void reserved_scalar_ftype_is_early_illegal()
{
	base::Harness harness;
	const auto bad_register = 0x1e20'4000U | (2U << 22) |
		(29U << 5) | 28U;
	const auto bad_immediate = 0x1e20'1000U | (2U << 22) |
		(0x55U << 13) | 28U;
	harness.require_illegal(bad_register, base::state(31U));
	harness.require_illegal(bad_immediate, base::state(32U));
}

} // namespace

int main()
{
	every_scalar_precision_and_general_variant();
	reserved_scalar_ftype_is_early_illegal();
	return 0;
}
