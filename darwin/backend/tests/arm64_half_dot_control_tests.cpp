// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_dot_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_half_dot_test;

namespace {

constexpr test::Registers registers{28U, 29U, 30U};

struct Profile final {
	std::uint32_t addend;
	std::uint16_t left_a;
	std::uint16_t left_b;
	std::uint16_t right_a;
	std::uint16_t right_b;
};

constexpr std::array profiles{
	Profile{0x0000'0001U, 0x0001U, 0x3c01U, 0x7c00U, 0xbc01U},
	Profile{0x3f80'0000U, 0x3555U, 0xb555U, 0x3c01U, 0x3bffU},
};

[[nodiscard]] test::arm64::CpuState uniform_state(
	const Profile &profile, std::uint32_t fpcr, unsigned seed)
{
	auto initial = test::state(seed);
	initial.fpcr = fpcr;
	initial.fpsr = 0x0800'0000U;
	for (unsigned lane = 0U; lane < 4U; ++lane) {
		test::exact::write_single(
			initial.vectors[registers.destination].bytes,
			lane, profile.addend);
		test::write_half(initial.vectors[registers.first].bytes,
			lane * 2U, profile.left_a);
		test::write_half(initial.vectors[registers.first].bytes,
			lane * 2U + 1U, profile.left_b);
		test::write_half(initial.vectors[registers.second].bytes,
			lane * 2U, profile.right_a);
		test::write_half(initial.vectors[registers.second].bytes,
			lane * 2U + 1U, profile.right_b);
	}
	return initial;
}

void every_control_combination_rounding_mode_and_form()
{
	constexpr std::array controls{
		test::fpcr_fiz, test::fpcr_ah, test::fpcr_fz16,
		test::fpcr_fz, test::fpcr_dn,
	};
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	unsigned seed = 8000U;
	for (const auto form : test::forms)
		for (unsigned mask = 0U; mask < (1U << controls.size());
		     ++mask)
			for (unsigned rounding = 0U; rounding < 4U;
			     ++rounding)
				for (unsigned profile = 0U;
				     profile < profiles.size(); ++profile) {
					std::uint32_t fpcr = rounding << 22U;
					for (unsigned bit = 0U;
					     bit < controls.size(); ++bit)
						if ((mask & (1U << bit)) != 0U)
							fpcr |= controls[bit];
					char name[192];
					std::snprintf(name, sizeof(name),
						"FDOT controls form=%u mask=%u "
						"rounding=%u profile=%u",
						static_cast<unsigned>(
							form.by_element) *
								8U +
							static_cast<unsigned>(
								form.wide) *
								4U +
							form.index,
						mask, rounding, profile);
					test::check(harness, name, form,
						registers,
						uniform_state(profiles[profile],
							fpcr, ++seed),
						counts);
				}
	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-dot-control-matrix");
}

void nan_priority_invalid_infinities_and_zero_sign()
{
	constexpr std::array edge_profiles{
		Profile{0U, 0x7e01U, 0x7c02U, 0x3c00U, 0x3c00U},
		Profile{0U, 0x7c01U, 0x7e02U, 0x3c00U, 0x3c00U},
		Profile{0U, 0x7c00U, 0x3c00U, 0x0000U, 0x3c00U},
		Profile{0U, 0x7c00U, 0xfc00U, 0x3c00U, 0x3c00U},
		Profile{0x7f80'0000U, 0xbc00U, 0x0000U,
			0x7c00U, 0x0000U},
		Profile{0U, 0x8000U, 0x0000U, 0x3c00U, 0xbc00U},
	};
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	unsigned seed = 15000U;
	for (const auto form : test::forms)
		for (const auto fpcr : {
			0U, test::fpcr_dn, test::fpcr_ah,
			test::fpcr_ah | test::fpcr_dn, 2U << 22U,
		})
			for (const auto &profile : edge_profiles)
				test::check(harness,
					"FDOT exceptional result class",
					form, registers,
					uniform_state(profile, fpcr, ++seed),
					counts);
	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-dot-control-special");
}

void specification_oracle_anchors()
{
	const auto signaling = test::dot_add(0U,
		0x7e01U, 0x7c02U, 0x3c00U, 0x3c00U, 0U);
	test::require(signaling.bits == 0x7fc0'4000U,
		"FDOT signaling-NaN priority anchor mismatch");
	test::require((signaling.flags & test::exact::fpsr_invalid) != 0U,
		"FDOT signaling-NaN did not set invalid");

	const auto flushed_addend = test::dot_add(1U,
		0U, 0U, 0U, 0U,
		test::fpcr_ah | test::fpcr_fz);
	test::require(flushed_addend.bits == 0U,
		"FDOT alternative post-round flush anchor mismatch");
	test::require((flushed_addend.flags &
		(test::exact::fpsr_input_denormal |
		 test::exact::fpsr_underflow |
		 test::exact::fpsr_inexact)) ==
		(test::exact::fpsr_input_denormal |
		 test::exact::fpsr_underflow |
		 test::exact::fpsr_inexact),
		"FDOT alternative flush flags anchor mismatch");
}

} // namespace

int main()
{
	specification_oracle_anchors();
	every_control_combination_rounding_mode_and_form();
	nan_priority_invalid_infinities_and_zero_sign();
	return 0;
}
