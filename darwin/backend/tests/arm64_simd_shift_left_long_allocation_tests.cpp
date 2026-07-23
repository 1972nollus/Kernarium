// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t instruction(bool high,
	bool unsigned_source, unsigned encoded)
{
	return 0x0f00'a400U | (high ? 0x4000'0000U : 0U) |
		(unsigned_source ? 0x2000'0000U : 0U) |
		((encoded & 127U) << 16) | (3U << 5) | 2U;
}

void every_immediate_and_form()
{
	test::Harness harness;
	/* immh == 0000 belongs to the modified-immediate allocation. */
	for (unsigned encoded = 8; encoded < 128; ++encoded)
		for (const bool high : {false, true})
			for (const bool unsigned_source : {false, true}) {
				const auto encoded_instruction = instruction(
					high, unsigned_source, encoded);
				if (encoded < 64U)
					harness.require_differential(encoded_instruction,
						test::state(encoded + high + unsigned_source));
				else
					harness.require_illegal(encoded_instruction,
						test::state(encoded + high + unsigned_source));
			}
}

} // namespace

int main()
{
	every_immediate_and_form();
	return 0;
}
