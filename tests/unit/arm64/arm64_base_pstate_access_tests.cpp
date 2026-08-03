// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_pstate_test_support.hpp"

#include <array>

namespace uml::emulator::tests::base_pstate {
namespace {

void denied_interrupt_mask_access(EngineHarness &harness)
{
	for (unsigned op2 : {6U, 7U})
		for (unsigned immediate = 0; immediate < 16U; ++immediate) {
			auto initial = seeded_state(
				0xd000U + op2 * 31U + immediate, false);
			initial.pstate_daif =
				static_cast<std::uint8_t>((immediate * 7U) & 15U);
			check_trap(harness,
				"base-pstate-interrupt-access-denied",
				encode(3U, immediate, op2), initial,
				NativeRequirements::baseline());
		}
}

void privileged_namespaces(EngineHarness &harness)
{
	for (unsigned immediate = 0; immediate < 16U; ++immediate) {
		const auto uao = decode(0U, immediate, 3U);
		const auto pan = decode(0U, immediate, 4U);
		const auto sp = decode(0U, immediate, 5U);
		arm64_system_test::require(uao.allocated &&
			pan.allocated && sp.allocated);
		check_trap(harness, "base-pstate-uao-privileged",
			encode(0U, immediate, 3U), seeded_state(immediate),
			NativeRequirements::features(ArmFeature::uao));
		check_trap(harness, "base-pstate-pan-privileged",
			encode(0U, immediate, 4U),
			seeded_state(0x100U + immediate),
			NativeRequirements::features(ArmFeature::pan));
		check_trap(harness, "base-pstate-sp-privileged",
			encode(0U, immediate, 5U),
			seeded_state(0x200U + immediate),
			NativeRequirements::baseline());
	}

	for (unsigned immediate = 0; immediate < 2U; ++immediate)
		check_trap(harness, "base-pstate-allint-privileged",
			encode(1U, immediate, 0U),
			seeded_state(0x300U + immediate),
			NativeRequirements::features(ArmFeature::nmi));

	constexpr std::array profiling_features{
		ArmFeature::ebep,
		ArmFeature::spe_exc,
		ArmFeature::trbe_exc,
	};
	for (const auto feature : profiling_features)
		for (unsigned immediate = 2U; immediate < 4U; ++immediate)
			check_trap(harness,
				"base-pstate-profiling-mask-privileged",
				encode(1U, immediate, 0U),
				seeded_state(0x400U +
					static_cast<unsigned>(feature) * 17U +
					immediate),
				NativeRequirements::features(feature));
}

void extension_namespaces(EngineHarness &harness)
{
	struct BooleanForm final {
		unsigned op2;
		ArmFeature feature;
	};
	constexpr std::array forms{
		BooleanForm{1U, ArmFeature::ssbs},
		BooleanForm{2U, ArmFeature::dit},
		BooleanForm{4U, ArmFeature::mte},
	};
	for (const auto form : forms)
		for (unsigned immediate : {0U, 1U, 14U, 15U}) {
			const auto decoded = decode(3U, immediate, form.op2);
			auto initial = seeded_state(
				0x600U + form.op2 * 31U + immediate);
			auto expected = initial;
			arm64_system_test::require(
				apply(expected, decoded.field, immediate));
			harness.check_one("base-pstate-extension-namespace",
				encode(3U, immediate, form.op2), initial,
				expected,
				NativeRequirements::features(form.feature));
		}

	for (unsigned immediate = 2U; immediate <= 7U; ++immediate) {
		const auto decoded = decode(3U, immediate, 3U);
		auto initial = seeded_state(0x700U + immediate);
		auto expected = initial;
		arm64_system_test::require(
			apply(expected, decoded.field, immediate));
		harness.check_one("base-pstate-sme-namespace",
			encode(3U, immediate, 3U), initial, expected,
			NativeRequirements::features(ArmFeature::sme));
	}

	/*
	 * With MTE2, TCO writes cannot take the MTE-only implementation-defined
	 * ignore choice. Exercise all high ignored operand bits under that exact
	 * native requirement as a separate architecture variant.
	 */
	for (unsigned immediate = 0; immediate < 16U; ++immediate) {
		const auto decoded = decode(3U, immediate, 4U);
		auto initial = seeded_state(0x900U + immediate);
		auto expected = initial;
		arm64_system_test::require(
			apply(expected, decoded.field, immediate));
		harness.check_one("base-pstate-mte2-namespace",
			encode(3U, immediate, 4U), initial, expected,
			NativeRequirements::features(
				{ArmFeature::mte, ArmFeature::mte2}));
	}
}

void vhe_decode_partition(EngineHarness &harness)
{
	for (unsigned op2 = 0; op2 < 8U; ++op2)
		for (unsigned immediate = 0; immediate < 16U; ++immediate) {
			const auto decoded = decode(5U, immediate, op2);
			arm64_system_test::require(!decoded.allocated);
			check_reserved(harness,
				"base-pstate-vhe-reserved-partition",
				encode(5U, immediate, op2),
				seeded_state(0xb000U +
					op2 * 17U + immediate));
		}
}

} // namespace
} // namespace uml::emulator::tests::base_pstate

int main()
{
	uml::emulator::tests::EngineHarness harness;
	uml::emulator::tests::base_pstate::denied_interrupt_mask_access(harness);
	uml::emulator::tests::base_pstate::privileged_namespaces(harness);
	uml::emulator::tests::base_pstate::extension_namespaces(harness);
	uml::emulator::tests::base_pstate::vhe_decode_partition(harness);
	harness.print_summary("arm64-base-pstate-access");
	return 0;
}
