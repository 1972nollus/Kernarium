// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * Translation-time histogram of guest SIMD operand registers (plan item G-4).
 *
 * G128 measured what vector residency is worth by switching it off in the JIT,
 * which already implements it: Ray Tracer +68.3%, Photo Filter +60.4%, Object
 * Detection +43.7%, Background Blur +32.0%. The gadget bank has no equivalent
 * and pays a CpuState round-trip on every guest SIMD operand, so the prize is
 * real and large.
 *
 * Whether the gadget engine can collect it is a different question. A gadget
 * body is assembled ahead of time and names its host registers statically, so a
 * fixed guest-to-host bank needs a separate gadget variant for each guest
 * register it covers. That is only affordable if translated code concentrates
 * its vector operands on a few registers, and how concentrated it is decides
 * both whether to build the bank and how wide to make it. G129 refuted the
 * alternative route - fusing adjacent SIMD pairs - by measuring that FP pairs
 * are 6.3% of translated adjacencies with no head, so this is the remaining
 * question rather than one of two.
 *
 * Compiled out entirely unless UML_GADGET_SIMD_OPERAND_HISTOGRAM is 1, so
 * ordinary translation has no diagnostic instrumentation, matching the
 * adjacent-pair histogram and the JIT's runtime diagnostics.
 *
 * Set DARWIN_UML_GADGET_SIMD_OPERAND_HISTOGRAM to a path to have the counts
 * written there. Counting is per thread into a fixed array and merged at dump
 * time, so translation stays lock-free and allocation-free after the first
 * record on a thread.
 */

#ifndef UML_GADGET_SIMD_OPERAND_HISTOGRAM
#define UML_GADGET_SIMD_OPERAND_HISTOGRAM 0
#endif

namespace uml::cpu::arm64::gadgets::diagnostics {

#if UML_GADGET_SIMD_OPERAND_HISTOGRAM
void record_simd_operand(unsigned index) noexcept;
#else
inline void record_simd_operand(unsigned) noexcept {}
#endif

} // namespace uml::cpu::arm64::gadgets::diagnostics
