// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/tlb.hpp"

#include <cstddef>
#include <cstdint>

namespace gadgets = uml::backend::arm64::gadgets;

static_assert(sizeof(gadgets::TlbEntry) == 32);
static_assert(alignof(gadgets::TlbEntry) == 32);
static_assert(offsetof(gadgets::Tlb, entries) == 0);
static_assert(offsetof(gadgets::Tlb, generation) ==
	gadgets::tlb_entry_count * sizeof(gadgets::TlbEntry));
static_assert(offsetof(gadgets::Tlb, changes) ==
	offsetof(gadgets::Tlb, generation) + sizeof(std::uint64_t));
static_assert(offsetof(gadgets::Tlb, fault_address) ==
	offsetof(gadgets::Tlb, changes) + sizeof(std::uint64_t));
static_assert(offsetof(gadgets::Tlb, fault_access) ==
	offsetof(gadgets::Tlb, fault_address) + sizeof(std::uint64_t));

int main()
{
	return 0;
}
