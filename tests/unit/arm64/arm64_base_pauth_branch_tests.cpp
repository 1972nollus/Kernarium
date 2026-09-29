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
}

} // namespace

int main()
{
	production_specialization();

	return 0;
}
