// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

#if defined(__APPLE__)
#define UML_GADGET_CEILING_ASM_NAME(value) __asm__(#value)
#else
#define UML_GADGET_CEILING_ASM_NAME(value)
#endif

extern "C" std::uint64_t uml_gadget_ceiling_native_workload(
	std::uint64_t iterations)
	UML_GADGET_CEILING_ASM_NAME(uml_gadget_ceiling_native_workload);
extern "C" const std::uint32_t uml_gadget_ceiling_body_begin[]
	UML_GADGET_CEILING_ASM_NAME(uml_gadget_ceiling_body_begin);
extern "C" const std::uint32_t uml_gadget_ceiling_body_end[]
	UML_GADGET_CEILING_ASM_NAME(uml_gadget_ceiling_body_end);

#undef UML_GADGET_CEILING_ASM_NAME
