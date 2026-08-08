// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

/*
 * Translation-time histogram of adjacent gadget pairs (plan item G-2 step 3).
 *
 * G111 fused guest CMP-immediate with a following B.cond and won 5.8% on
 * P-cores and 6.5% on E-cores. G112 then fused the pair that looked like the
 * obvious next one - SUBS with a live destination, the counted-loop shape - and
 * lost on Geekbench despite winning on its probe. The lesson is that a pair's
 * value is not visible by eye: it depends on how often the pair actually occurs
 * in translated code and on what the fused entry costs the working set, neither
 * of which is guessable.
 *
 * This records what really gets translated, so further pairs are chosen by
 * frequency rather than by intuition. It is compiled out entirely unless
 * UML_GADGET_PAIR_HISTOGRAM is 1, so production translation is byte-identical -
 * the same discipline G49's pressure counters and the JIT's runtime diagnostics
 * already follow.
 *
 * Set DARWIN_UML_GADGET_PAIR_HISTOGRAM to a path to have the counts written
 * there at exit, most-frequent first, with symbol names resolved through
 * dladdr. Counting is per thread and merged at dump time, so translation stays
 * lock-free.
 */

#ifndef UML_GADGET_PAIR_HISTOGRAM
#define UML_GADGET_PAIR_HISTOGRAM 0
#endif

namespace uml::cpu::arm64::gadgets::diagnostics {

#if UML_GADGET_PAIR_HISTOGRAM
void record_gadget_pair(const void *previous, const void *current) noexcept;
#else
inline void record_gadget_pair(const void *, const void *) noexcept {}
#endif

} // namespace uml::cpu::arm64::gadgets::diagnostics
