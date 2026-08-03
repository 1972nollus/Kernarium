// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_hint_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

namespace engine_tests = uml::emulator::tests;
using arm64_hint_test::ExpectedCounts;
using arm64_hint_test::Harness;
using ::uml::cpu::ArmFeature;

[[nodiscard]] engine_tests::NativeRequirements
spec_primary_requirements(unsigned immediate)
{
	switch (immediate) {
	case 6:
		return engine_tests::NativeRequirements::features(
			ArmFeature::dgh);
	case 16:
		/*
		 * FEAT_E3DSE changes one internal ESB action but is not an
		 * allocation prerequisite. The exact ESB gate remains FEAT_RAS.
		 */
		return engine_tests::NativeRequirements::features(
			ArmFeature::ras);
	case 17:
		return engine_tests::NativeRequirements::features(
			ArmFeature::spe);
	case 18:
		return engine_tests::NativeRequirements::features(
			ArmFeature::trf);
	case 19:
		return engine_tests::NativeRequirements::features(
			ArmFeature::gcs);
	case 22:
		return engine_tests::NativeRequirements::features(
			ArmFeature::clrbhb);
	case 32:
	case 34:
	case 36:
	case 38:
		return engine_tests::NativeRequirements::features(
			ArmFeature::bti);
	case 40:
		return engine_tests::NativeRequirements::features(
			ArmFeature::chk);
	case 48:
	case 49:
		return engine_tests::NativeRequirements::features(
			ArmFeature::pcdphint);
	case 50:
	case 51:
	case 52:
		return engine_tests::NativeRequirements::features(
			ArmFeature::cmh);
	default:
		return engine_tests::NativeRequirements::baseline();
	}
}

void exact_requirement_table()
{
	for (unsigned immediate = 0; immediate < 128U; ++immediate) {
		if (!arm64_hint_test::is_owned_primary_hint(immediate))
			continue;
		const auto actual =
			arm64_hint_test::primary_requirements(immediate);
		const auto expected = spec_primary_requirements(immediate);
		arm64_hint_test::require(
			actual.features() == expected.features(),
			"primary HINT has an incorrect native feature gate");
	}
}

void execute_every_feature_variant(Harness &harness,
	ExpectedCounts &expected)
{
	/*
	 * Keep every encoding version here, even where several variants share a
	 * feature. This makes a wrong per-variant requirement visible through the
	 * harness's exact run/skip counts.
	 */
	constexpr std::array immediates{
		6U, 16U, 17U, 18U, 19U, 22U,
		32U, 34U, 36U, 38U,
		50U, 51U,
	};
	for (const unsigned immediate : immediates) {
		const auto instruction =
			arm64_hint_test::primary_instruction(immediate);
		const auto initial = arm64_hint_test::seeded_state(
			0x7100'0000'0000'0000ULL |
			(static_cast<std::uint64_t>(immediate) << 17));
		char name[128];
		std::snprintf(name, sizeof(name),
			"BASE-0175 HINT exact feature gate immediate=%u",
			immediate);
		const auto native =
			arm64_hint_test::primary_requirements(immediate);
		harness.check_stateless(name, instruction, initial, native);
		expected.add(native);
	}

	/*
	 * When E3DSE is present, ESB has an additional internal synchronization
	 * action. It remains architecturally state-invisible in this EL0 model,
	 * but this distinct operational version may use native JIT execution only
	 * on a host implementing both prerequisites.
	 */
	const auto e3dse_native =
		engine_tests::NativeRequirements::features(ArmFeature::ras) |
		ArmFeature::e3dse;
	const auto e3dse_initial = arm64_hint_test::seeded_state(
		0x7110'0000'0000'0000ULL);
	harness.check_stateless(
		"BASE-0175 HINT ESB FEAT_E3DSE operational version",
		arm64_hint_test::primary_instruction(16),
		e3dse_initial, e3dse_native);
	expected.add(e3dse_native);
}

void standalone_feature_gates(Harness &harness, ExpectedCounts &expected)
{
	const auto sb_native = engine_tests::NativeRequirements::features(
		ArmFeature::sb);
	const auto sb_initial = arm64_hint_test::seeded_state(
		0x7200'0000'0000'0000ULL);
	harness.check_stateless("BASE-0356 SB exact FEAT_SB gate",
		arm64_hint_test::speculation_barrier_instruction,
		sb_initial, sb_native);
	expected.add(sb_native);

	const auto gcie_native = engine_tests::NativeRequirements::features(
		ArmFeature::gcie);
	for (unsigned operation = 0; operation < 2U; ++operation)
		for (unsigned transfer = 0; transfer < 32U; ++transfer) {
			const auto instruction =
				arm64_hint_test::gsb_instruction(
					operation, transfer);
			const auto initial = arm64_hint_test::seeded_state(
				0x7210'0000'0000'0000ULL |
				(operation << 8) | transfer);
			harness.check_profile_illegal(
				operation == 0U ?
					"BASE-0174 GSB SYS exact FEAT_GCIE gate" :
					"BASE-0174 GSB ACK exact FEAT_GCIE gate",
				instruction, initial, gcie_native);
			expected.add(gcie_native);
		}

	const auto baseline = engine_tests::NativeRequirements::baseline();
	const auto baseline_initial = arm64_hint_test::seeded_state(
		0x7220'0000'0000'0000ULL);
	harness.check_stateless("BASE-0137 CSDB baseline JIT gate",
		arm64_hint_test::primary_instruction(20),
		baseline_initial, baseline);
	expected.add(baseline);
}

} // namespace

int main()
{
	exact_requirement_table();
	Harness harness;
	ExpectedCounts expected;
	execute_every_feature_variant(harness, expected);
	standalone_feature_gates(harness, expected);
	arm64_hint_test::require_counts(harness, expected);
	harness.print_summary("arm64-base-hint-features");
	return 0;
}
