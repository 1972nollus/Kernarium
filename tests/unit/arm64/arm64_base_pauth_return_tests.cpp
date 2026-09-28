// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

#include <set>

namespace pauth = arm64_pauth_test;

namespace {

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool key_b : {false, true}) {
		const auto built = pauth::gadgets::Builder{}.build(
			pauth::pc,
			std::array{
				pauth::authenticated_return(key_b)});
		pauth::require(
			built.complete &&
			built.program.instruction_count() == 1);
		entries.insert(built.program.words().front());
	}
	pauth::require(entries.size() == 2);
	std::set<std::uintptr_t> guarded_entries;
	for (const bool key_b : {false, true}) {
		const auto built = pauth::gadgets::Builder{}.build(
			pauth::pc,
			std::array{pauth::authenticated_return(key_b)},
			{}, std::pmr::get_default_resource(), {},
			pauth::gadgets::BuildOptions{
				.enable_guarded_control_flow = true});
		pauth::require(
			built.complete &&
			built.program.instruction_count() == 1);
		const auto entry = built.program.words().front();
		pauth::require(entries.count(entry) == 0);
		guarded_entries.insert(entry);
	}
	pauth::require(guarded_entries.size() == 2);

	const auto eretaa = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{0xd69f'0bffU});
	const auto eretab = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{0xd69f'0fffU});
	const auto illegal = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{std::uint32_t{0}});
	pauth::require(
		eretaa.program.words().front() ==
			illegal.program.words().front() &&
		eretab.program.words().front() ==
			illegal.program.words().front());
}

/* The guarded return used to park its target in the XZR slot, even with the
 * control stack disabled. */
void guarded_return_keeps_zero_register()
{
	constexpr auto target = pauth::pc + 0x100;
	constexpr std::array branch{
		pauth::hint(25), // paciasp
		pauth::authenticated_return(false), // retaa
	};
	pauth::arm64::CpuState cpu{};
	cpu.registers[30] = target;
	cpu.sp = 0x7000'0000;
	pauth::guarded_branch_keeps_zero_register(branch, cpu, target);
}

} // namespace

int main()
{
	production_specialization();
	guarded_return_keeps_zero_register();

	return 0;
}
