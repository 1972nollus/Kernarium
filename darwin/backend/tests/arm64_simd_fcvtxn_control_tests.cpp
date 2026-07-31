// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_precision_conversion_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_precision_conversion_test;

namespace {

void fpcr_controls_nep_and_sticky_status()
{
	test::engine_tests::EngineHarness harness;
	constexpr std::array controls{
		0U, test::fpcr_fz, test::fpcr_fiz, test::fpcr_ah,
		test::fpcr_ah | test::fpcr_fz,
		test::fpcr_ah | test::fpcr_fiz,
		test::fpcr_dn, test::fpcr_dn | test::fpcr_ah,
		test::fpcr_fz16, test::fpcr_nep,
		test::fpcr_nep | test::fpcr_ah,
	};
	constexpr std::array inputs{
		0x0000'0000'0000'0001ULL,
		0x8000'0000'0000'0001ULL,
		0x7ff0'0000'0000'0000ULL,
		0x7ff8'1234'5678'9abcULL,
		0x7ff0'1234'5678'9abcULL,
	};
	unsigned seed = 3'500U;
	for (const auto form : test::round_odd_forms) {
		for (const auto fpcr : controls) {
			for (unsigned index = 0U; index < inputs.size(); ++index) {
				auto initial = test::state(++seed);
				initial.fpcr = fpcr;
				initial.fpsr = test::fpsr_overflow | (1U << 27);
				const std::array values{inputs[index]};
				test::fill_source(initial, form, {28U, 29U}, values);
				char name[176];
				std::snprintf(name, sizeof(name),
					"floating-round-odd-control scalar=%u "
					"upper=%u fpcr=%x input=%u",
					static_cast<unsigned>(form.scalar),
					static_cast<unsigned>(form.upper), fpcr, index);
				test::check(harness, name, form, {28U, 29U},
					initial);
			}
		}
	}
	harness.print_summary("arm64-simd-fcvtxn-control");
}

} // namespace

int main()
{
	fpcr_controls_nep_and_sticky_status();
	return 0;
}
