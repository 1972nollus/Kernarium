// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

#include <set>

namespace pauth = arm64_pauth_test;

namespace {

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (unsigned operation = 0; operation < 8; ++operation) {
		for (const bool zero_modifier : {false, true}) {
			const auto built = pauth::gadgets::Builder{}.build(
				pauth::pc,
				std::array{pauth::data_processing(
					operation | (zero_modifier ? 8U : 0U),
					3, zero_modifier ? 31U : 7U)});
			pauth::require(
				built.complete &&
				built.program.instruction_count() == 1 &&
				built.program.words().size() == 4);
			entries.insert(built.program.words().front());
		}
	}
	pauth::require(entries.size() == 16);

	const auto discard = pauth::gadgets::Builder{}.build(
		pauth::pc,
		std::array{pauth::data_processing(0, 31, 7)});
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
