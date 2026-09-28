// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

#include <set>

namespace pauth = arm64_pauth_test;

namespace {

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool link : {false, true}) {
		for (const bool key_b : {false, true}) {
			for (const bool target_zero : {false, true}) {
				for (const bool modifier_zero : {false, true}) {
					const auto built =
						pauth::gadgets::Builder{}.build(
							pauth::pc,
							std::array{
								pauth::authenticated_branch(
									link, key_b,
									modifier_zero,
									target_zero ? 31U : 3U,
									modifier_zero ? 31U :
										7U)});
					pauth::require(
						built.complete &&
						built.program.instruction_count() ==
							1);
					entries.insert(
						built.program.words().front());
				}
			}
		}
	}
	pauth::require(entries.size() == 16);

	std::set<std::uintptr_t> guarded_entries;
	for (const bool key_b : {false, true}) {
		for (const bool target_zero : {false, true}) {
			for (const bool modifier_zero : {false, true}) {
				const auto built =
					pauth::gadgets::Builder{}.build(
						pauth::pc,
						std::array{
							pauth::authenticated_branch(
								true, key_b,
								modifier_zero,
								target_zero ? 31U :
									3U,
								modifier_zero ? 31U :
									7U)},
						{}, std::pmr::get_default_resource(),
						{},
						pauth::gadgets::BuildOptions{
							.enable_guarded_control_flow =
								true});
				pauth::require(
					built.complete &&
					built.program.instruction_count() == 1);
				const auto entry = built.program.words().front();
				pauth::require(entries.count(entry) == 0);
				guarded_entries.insert(entry);
			}
		}
	}
	pauth::require(guarded_entries.size() == 8);
}

/* The guarded call used to park its target in the XZR slot. */
void guarded_call_keeps_zero_register()
{
	constexpr auto target = pauth::pc + 0x100;
	constexpr std::array branch{
		pauth::data_processing(8, 16), // paciza x16
		pauth::authenticated_branch(true, false, true, 16), // blraaz x16
	};
	pauth::arm64::CpuState cpu{};
	cpu.registers[16] = target;
	pauth::guarded_branch_keeps_zero_register(branch, cpu, target);
}

} // namespace

int main()
{
	production_specialization();
	guarded_call_keeps_zero_register();

	return 0;
}
