// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

#include <set>

namespace pauth = arm64_pauth_test;

namespace {

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const auto instruction :
	     {pauth::xpac(false, 3), pauth::xpac(true, 3),
	      std::uint32_t{0xd503'20ffU}}) {
		const auto built = pauth::gadgets::Builder{}.build(
			pauth::pc, std::array{instruction});
		pauth::require(
			built.complete &&
			built.program.instruction_count() == 1);
		entries.insert(built.program.words().front());
	}
	pauth::require(entries.size() == 3);
	const auto discard = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{pauth::xpac(false, 31)});
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
