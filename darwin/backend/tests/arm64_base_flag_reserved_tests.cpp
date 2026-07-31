// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_flag_test_support.hpp"

#include <array>
#include <cstdint>

namespace uml::backend::tests::base_flag {
namespace {

void rmif_reserved_fixed_bits(EngineHarness &harness)
{
	/* Bit 10 is deliberately excluded: clearing it names an allocated
	 * add-with-carry form rather than a reserved member of this allocation.
	 */
	constexpr std::array fixed_bits{4U, 11U, 12U, 13U, 14U};
	for (unsigned source = 0; source < 32U; ++source)
		for (const auto bit : fixed_bits) {
			const auto instruction =
				encode_rmif(source, source * 7U, source) ^
				(1U << bit);
			require(!is_rmif(instruction),
				"reserved RMIF encoding still matches allocation");
			auto initial = seeded_state(
				0x3500'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 9U));
			harness.check_illegal("BASE-0351 RMIF reserved fixed bit",
				instruction, initial,
				NativeRequirements::features(ArmFeature::flag_m));
		}
}

void setf_reserved_fixed_bits(EngineHarness &harness)
{
	/* Bit 11 is the neighboring add-with-carry allocation when cleared. */
	constexpr std::array fixed_bits{4U, 10U, 12U, 13U, 15U};
	for (const bool sixteen : {false, true})
		for (unsigned source = 0; source < 32U; ++source)
			for (const auto bit : fixed_bits) {
				const auto instruction = encode_setf(sixteen, source) ^
					(1U << bit);
				require(!is_setf(instruction),
					"reserved SETF encoding still matches allocation");
				auto initial = seeded_state(
					0x3510'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(
						instruction) << 7U));
				harness.check_illegal(
					"BASE-0363 SETF reserved fixed bit",
					instruction, initial,
					NativeRequirements::features(
						ArmFeature::flag_m));
			}
}

} // namespace
} // namespace uml::backend::tests::base_flag

int main()
{
	uml::backend::tests::EngineHarness harness;
	uml::backend::tests::base_flag::rmif_reserved_fixed_bits(harness);
	uml::backend::tests::base_flag::setf_reserved_fixed_bits(harness);
	harness.print_summary("arm64-base-flag-reserved");
	return 0;
}
