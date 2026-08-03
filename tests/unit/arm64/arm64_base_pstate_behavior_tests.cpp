// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_pstate_test_support.hpp"

#include <array>

namespace uml::emulator::tests::base_pstate {
namespace {

void boolean_fields(EngineHarness &harness)
{
	struct Form final {
		unsigned op2;
		Field field;
	};
	constexpr std::array forms{
		Form{1U, Field::ssbs},
		Form{2U, Field::dit},
		Form{4U, Field::tag_check_override},
	};
	for (const auto form : forms)
		for (unsigned immediate = 0; immediate < 16U; ++immediate)
			for (const bool initial_value : {false, true}) {
				auto initial = seeded_state(
					(immediate << 8U) | form.op2);
				switch (form.field) {
				case Field::ssbs:
					initial.pstate_ssbs = initial_value;
					break;
				case Field::dit:
					initial.pstate_dit = initial_value;
					break;
				case Field::tag_check_override:
					initial.pstate_tco = initial_value;
					break;
				default:
					arm64_system_test::require(false);
				}
				const auto decoded = decode(3U, immediate,
					form.op2);
				check_allocated(harness,
					"base-pstate-boolean-behavior",
					encode(3U, immediate, form.op2),
					initial, decoded, immediate);
			}
}

void streaming_state(EngineHarness &harness)
{
	for (unsigned immediate = 2U; immediate <= 7U; ++immediate)
		for (unsigned initial_bits = 0; initial_bits < 4U;
		     ++initial_bits) {
			auto initial = seeded_state(
				0x500U + immediate * 17U + initial_bits);
			initial.pstate_sm = (initial_bits & 1U) != 0;
			initial.pstate_za = (initial_bits & 2U) != 0;
			const auto decoded = decode(3U, immediate, 3U);
			check_allocated(harness,
				(immediate & 1U) != 0 ?
					"BASE-0382 SMSTART streaming-alias-behavior" :
					"BASE-0383 SMSTOP streaming-alias-behavior",
				encode(3U, immediate, 3U), initial,
				decoded, immediate);
		}
}

void interrupt_masks(EngineHarness &harness)
{
	for (unsigned op2 : {6U, 7U})
		for (unsigned immediate = 0; immediate < 16U; ++immediate)
			for (unsigned initial_mask = 0; initial_mask < 16U;
			     ++initial_mask) {
				auto initial = seeded_state(
					0x800U + op2 * 257U +
					immediate * 17U + initial_mask,
					true);
				initial.pstate_daif =
					static_cast<std::uint8_t>(initial_mask);
				const auto decoded =
					decode(3U, immediate, op2);
				check_allocated(harness,
					op2 == 6U ?
						"base-pstate-interrupt-set" :
						"base-pstate-interrupt-clear",
					encode(3U, immediate, op2),
					initial, decoded, immediate);
			}
}

} // namespace
} // namespace uml::emulator::tests::base_pstate

int main()
{
	uml::emulator::tests::EngineHarness harness;
	uml::emulator::tests::base_pstate::boolean_fields(harness);
	uml::emulator::tests::base_pstate::streaming_state(harness);
	uml::emulator::tests::base_pstate::interrupt_masks(harness);
	harness.print_summary("arm64-base-pstate-behavior");
	return 0;
}
