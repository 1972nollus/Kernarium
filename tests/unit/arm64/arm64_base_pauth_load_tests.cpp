// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

#include "uml_darwin_guest.h"

#include <cstring>
#include <set>
#include <vector>

namespace pauth = arm64_pauth_test;

namespace {

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool key_b : {false, true}) {
		for (const bool writeback : {false, true}) {
			for (const bool base_sp : {false, true}) {
				for (const bool discard : {false, true}) {
					const auto built =
						pauth::gadgets::Builder{}.build(
							pauth::pc,
							std::array{
								pauth::authenticated_load(
									key_b, writeback, 0,
									base_sp ? 31U : 3U,
									discard ? 31U : 7U)});
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
