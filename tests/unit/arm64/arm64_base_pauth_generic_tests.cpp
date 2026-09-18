// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

#include <set>

namespace pauth = arm64_pauth_test;

namespace {

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool source_zero : {false, true}) {
		const auto built = pauth::gadgets::Builder{}.build(
			pauth::pc,
			std::array{pauth::pacga(
				3, source_zero ? 31U : 7U, 11)});
		pauth::require(
			built.complete &&
			built.program.instruction_count() == 1 &&
			built.program.words().size() == 4);
		entries.insert(built.program.words().front());
	}
	pauth::require(entries.size() == 2);
	const auto discard = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{pauth::pacga(31, 7, 11)});
	const auto nop = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{pauth::hint(0)});
	pauth::require(discard.program.words().front() ==
		nop.program.words().front());
}

} // namespace

int main()
{
	production_specialization();
	return 0;
}
