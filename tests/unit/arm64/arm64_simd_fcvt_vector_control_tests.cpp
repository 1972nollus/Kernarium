// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_precision_conversion_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_precision_conversion_test;

namespace {

[[nodiscard]] constexpr std::array<std::uint64_t, 6>
control_inputs(unsigned width) noexcept
{
	if (width == 16U)
		return {0x0001U, 0x8001U, 0x7c00U, 0x7e55U,
			0x7d55U, 0xfc00U};
	if (width == 32U)
		return {0x0000'0001U, 0x8000'0001U, 0x7f80'0000U,
			0x7fc1'2345U, 0x7f81'2345U, 0xff80'0000U};
	return {0x0000'0000'0000'0001ULL,
		0x8000'0000'0000'0001ULL,
		0x7ff0'0000'0000'0000ULL,
		0x7ff8'1234'5678'9abcULL,
		0x7ff0'1234'5678'9abcULL,
		0xfff0'0000'0000'0000ULL};
}

void fpcr_controls_status_stickiness_and_alternative_half()
{
	test::engine_tests::EngineHarness harness;
	constexpr std::array controls{
		0U, test::fpcr_fz, test::fpcr_fiz, test::fpcr_ah,
		test::fpcr_ah | test::fpcr_fz,
		test::fpcr_ah | test::fpcr_fiz,
		test::fpcr_dn, test::fpcr_dn | test::fpcr_ah,
		test::fpcr_fz16, test::fpcr_ahp,
		test::fpcr_ahp | test::fpcr_dn,
	};
	unsigned seed = 1'000U;
	for (const auto form : test::ordinary_forms) {
		for (const auto fpcr : controls) {
			const auto inputs = control_inputs(form.source_width);
			for (unsigned index = 0U; index < inputs.size(); ++index) {
				auto initial = test::state(++seed);
				initial.fpcr = fpcr;
				initial.fpsr = test::fpsr_overflow | (1U << 27);
				const std::array values{inputs[index]};
				test::fill_source(initial, form, {28U, 29U}, values);
				char name[192];
				std::snprintf(name, sizeof(name),
					"floating-precision-control operation=%u "
					"source=%u destination=%u upper=%u fpcr=%x "
					"input=%u",
					static_cast<unsigned>(form.operation),
					form.source_width, form.destination_width,
					static_cast<unsigned>(form.upper), fpcr, index);
				test::check(harness, name, form, {28U, 29U},
					initial);
			}
		}
	}
	harness.print_summary("arm64-simd-fcvt-vector-control");
}

} // namespace

int main()
{
	fpcr_controls_status_stickiness_and_alternative_half();
	return 0;
}
