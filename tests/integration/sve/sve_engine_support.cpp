// SPDX-License-Identifier: GPL-2.0-or-later
/* Prints how much of the non-streaming SVE profile each execution engine
 * implements, as "gadgets=<level> jit=<level>", for
 * tests/integration/sve/run_guest_sve.py to decide whether a guest SVE test
 * can run on an engine. */

#include "cpu/arm64/gadgets/sve_support.hpp"
#include "cpu/arm64/jit/jit_sve_support.hpp"

#include <cstdio>

namespace {

const char *level(uml::cpu::arm64::SveSupport support)
{
	switch (support) {
	case uml::cpu::arm64::SveSupport::none:
		return "none";
	case uml::cpu::arm64::SveSupport::control:
		return "control";
	case uml::cpu::arm64::SveSupport::complete:
		return "complete";
	}
	return "unknown";
}

} // namespace

int main()
{
	std::printf("gadgets=%s jit=%s\n",
		level(uml::cpu::arm64::gadgets::sve_support),
		level(uml::cpu::arm64::jit::sve_support));
	return 0;
}
