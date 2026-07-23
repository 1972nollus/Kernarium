// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_cvtfi_test_support.hpp"

namespace test = arm64_simd_cvtfi_test;
namespace base = arm64_simd_test;

int main()
{
	base::Harness harness;
	constexpr std::array positive{0x4b80'0000U, 0x4b80'0001U,
		0x4b80'0000U, 0x4b80'0000U};
	constexpr std::array negative{0xcb80'0000U, 0xcb80'0000U,
		0xcb80'0001U, 0xcb80'0000U};
	for (unsigned rounding = 0; rounding < 4U; ++rounding) {
		auto unsigned_initial = base::state(rounding);
		unsigned_initial.fpcr = rounding << 22;
		unsigned_initial.registers[29] = 16'777'217U;
		const std::array unsigned_output{
			static_cast<std::uint64_t>(positive[rounding])};
		test::require_vector(test::gpr_instruction(true, 64U, 32U,
			28U, 29U), 32U, 4U, true, 28U, unsigned_output,
			1U << 4, unsigned_initial, harness);

		auto signed_initial = base::state(10U + rounding);
		signed_initial.fpcr = rounding << 22;
		signed_initial.registers[29] =
			static_cast<std::uint64_t>(-16'777'217LL);
		const std::array signed_output{
			static_cast<std::uint64_t>(negative[rounding])};
		test::require_vector(test::gpr_instruction(false, 64U, 32U,
			28U, 29U), 32U, 4U, true, 28U, signed_output,
			1U << 4, signed_initial, harness);
	}
	return 0;
}
