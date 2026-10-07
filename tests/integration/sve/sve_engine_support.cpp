// SPDX-License-Identifier: GPL-2.0-or-later
/* Prints how much of the non-streaming SVE profile each execution engine
 * implements, and whether the guest kernel keeps SVE task state, as
 * "gadgets=<level> jit=<level> kernel=<yes|no>", for
 * tests/integration/sve/run_guest_sve.py to decide whether a guest SVE test
 * can run on an engine. */

#include "cpu/arm64/gadgets/sve_support.hpp"
#include "cpu/arm64/jit/jit_sve_support.hpp"
#include "uml_darwin_abi.h"

#include <cstdio>

namespace {

/* The build accepts only a kernel artifact of the bridge ABI it declares,
 * so the kernel keeps SVE task state exactly when that ABI has the SVE
 * registers in the arm64 task tail. */
#ifdef DARWIN_UML_ABI_GUEST_SVE_REGISTERS_OFFSET
constexpr const char *kernel_sve = "yes";
#else
constexpr const char *kernel_sve = "no";
#endif

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
	std::printf("gadgets=%s jit=%s kernel=%s\n",
		level(uml::cpu::arm64::gadgets::sve_support),
		level(uml::cpu::arm64::jit::sve_support), kernel_sve);
	return 0;
}
