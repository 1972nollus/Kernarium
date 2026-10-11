// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

/* darwin/cpu/CMakeLists.txt defines both for every includer. A default here
 * would let a translation unit see a geometry other engines do not. */
#ifndef UML_GADGET_CACHE_SLOTS
#error "UML_GADGET_CACHE_SLOTS must come from the build"
#endif
#ifndef UML_GADGET_RESIDENT_GENERATIONS
#error "UML_GADGET_RESIDENT_GENERATIONS must come from the build"
#endif

namespace uml::cpu::gadget_cache {

/* Both ISAs keep executor-owned links in identically shaped private caches.
 * The translated payload and its validation remain owned by the ISA. */
inline constexpr std::size_t slots = UML_GADGET_CACHE_SLOTS;
inline constexpr std::size_t ways = 4;
inline constexpr std::size_t slab_slots = 256;
inline constexpr std::size_t resident_generations = UML_GADGET_RESIDENT_GENERATIONS;
static_assert((slots & (slots - 1U)) == 0);
static_assert(slots >= slab_slots * 4U && slots % slab_slots == 0);
static_assert(slots % ways == 0);
static_assert(resident_generations != 0);

/* An address space sweeps its link records and indirect sites once they
 * number half again what its last sweep kept, and not before they number
 * `sweep_floor`: a sweep reads every stream the address space holds, and
 * short-lived address spaces never make that many. */
inline constexpr std::size_t sweep_floor = 4096;

[[nodiscard]] constexpr std::size_t next_sweep(std::size_t kept) noexcept
{
	return kept + kept / 2 > sweep_floor ? kept + kept / 2 : sweep_floor;
}

} // namespace uml::cpu::gadget_cache
