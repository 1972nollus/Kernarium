/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Keep one translation unit so shared macros and local labels remain cheap. */
/* Zero-size bounds of the bank and of each section its gadgets push code
 * into, for the guest sampler's PC test. They emit nothing, so no gadget
 * moves, and the sections are named here in the order the bank already
 * creates them. NAME() is not defined until core_integer.inc. */
.macro gadget_bank_bound name
#if __APPLE__
	.globl _\name
_\name:
#else
	.globl \name
\name:
#endif
.endm

.macro gadget_bank_bounds edge
	.text
	gadget_bank_bound uml_arm64_gadget_bank_\edge
#if __APPLE__
	.pushsection __TEXT,__gadget_forward,regular,pure_instructions
#else
	.pushsection .text.gadget_forward,"ax"
#endif
	gadget_bank_bound uml_arm64_gadget_forward_section_\edge
	.popsection
#if __APPLE__
	.pushsection __TEXT,__vector_forward,regular,pure_instructions
#else
	.pushsection .text.vector_forward,"ax"
#endif
	gadget_bank_bound uml_arm64_gadget_vector_forward_section_\edge
	.popsection
#if __APPLE__
	.pushsection __TEXT,__gadget_cold,regular,pure_instructions
#else
	.pushsection .text.gadget_cold,"ax"
#endif
	gadget_bank_bound uml_arm64_gadget_cold_section_\edge
	.popsection
.endm

	gadget_bank_bounds begin
#include "engine/core_integer.inc"
/* Keep the FP/SIMD bank on its established 64-byte cache-line phase after
 * factoring the native-environment cold path into one shared routine. */
.space 16
#include "engine/floating_simd.inc"
#include "engine/system_atomic.inc"
#include "engine/load_store.inc"
#include "engine/control.inc"
/* Experimental entries are appended so adding a candidate cannot move or
 * rephase any established gadget. */
#include "engine/experimental.inc"
#include "engine/inplace_immediate.inc"
#include "engine/inplace_extended.inc"
#include "engine/template_copies.inc"
#include "engine/inplace_memory.inc"
#include "engine/scalar_forwarding.inc"
#include "engine/vector_forwarding.inc"
#include "engine/dependency_coverage.inc"
	gadget_bank_bounds end
