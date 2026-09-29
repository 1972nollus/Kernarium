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

} // namespace

int main()
{
	production_specialization();

	return 0;
}
