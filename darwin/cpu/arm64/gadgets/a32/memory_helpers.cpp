// SPDX-License-Identifier: GPL-2.0-or-later
//
// The page-crossing store of STM and PUSH in the AArch32 gadget part
// (generate_a32.py, block_gadget). A store multiple is a sequence of word
// stores: when the second page is not writable, the words ahead of the
// fault are in memory and the fault is reported at the first word of that
// page, with the base register and the other registers as they were, so
// that the instruction restarts. A load multiple does not come here; it
// reads into a buffer through uml_arm64_memory_transfer and changes no
// register unless every page was readable.
#include "memory/tlb.hpp"
#include "uml_darwin_guest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" std::uint32_t uml_arm64_a32_store_block(
	::uml::memory::Tlb *tlb, std::uint64_t address, const void *data,
	std::size_t size) noexcept
{
	constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
	if (tlb == nullptr || data == nullptr || size == 0 || size > 64)
		return 0;
	const auto *bytes = static_cast<const std::byte *>(data);
	std::size_t completed = 0;
	while (completed < size) {
		const auto current = address + completed;
		auto *translated = tlb->translate_tagged(
			current, ::uml::memory::Access::write);
		if (translated == nullptr)
			return 0;
		const auto length = std::min(size - completed,
			page_size - static_cast<std::size_t>(current % page_size));
		std::memcpy(translated, bytes + completed, length);
		completed += length;
	}
	return 1;
}
