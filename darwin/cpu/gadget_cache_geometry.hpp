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

} // namespace uml::cpu::gadget_cache
