// SPDX-License-Identifier: GPL-2.0-or-later
/* Prints how much of the AArch32 profile each execution engine implements, as
 * "gadgets=<level> jit=<level>", for run_guest_a32_engine.py to decide
 * whether the engine probe can run on an engine. */

#include "cpu/arm64/gadgets/a32_support.hpp"
#include "cpu/arm64/jit/jit_a32_support.hpp"

#include <cstdio>

namespace {

const char *level(uml::cpu::arm64::A32Support support)
{
	switch (support) {
	case uml::cpu::arm64::A32Support::none:
		return "none";
	case uml::cpu::arm64::A32Support::base:
		return "base";
	case uml::cpu::arm64::A32Support::simd:
		return "simd";
	case uml::cpu::arm64::A32Support::complete:
		return "complete";
	}
	return "unknown";
}

} // namespace

int main()
{
	std::printf("gadgets=%s jit=%s\n",
		level(uml::cpu::arm64::gadgets::a32_support),
		level(uml::cpu::arm64::jit::a32_support));
	return 0;
}
