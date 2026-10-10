// SPDX-License-Identifier: GPL-2.0-or-later
//
// The page-crossing multiple transfers of the VFP part
// (engine/a32_simd.py, vfp_block): VLDM, VSTM, VPUSH and VPOP. The
// AArch32 semantic library covers the base profile only, so until it carries
// VFP these are the part's own. A store multiple is a sequence of word
// stores: when the second page is not writable, the words ahead of the fault
// are in memory and the fault is reported at the first word of that page,
// with the base register and the other registers as they were, so that the
// instruction restarts. A load multiple reads into a buffer and changes no
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
	if (tlb == nullptr || data == nullptr || size == 0 || size > 192)
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

/* A load of up to 192 bytes, across at most one page boundary, into `data`.
 * Every page is translated before any byte is read, and nothing is read
 * when one cannot be. */
extern "C" std::uint32_t uml_arm64_a32_load_block(
	::uml::memory::Tlb *tlb, std::uint64_t address, void *data,
	std::size_t size) noexcept
{
	constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
	if (tlb == nullptr || data == nullptr || size == 0 || size > 192)
		return 0;
	const std::byte *pages[2]{};
	std::size_t lengths[2]{};
	std::size_t count = 0;
	std::size_t completed = 0;
	while (completed < size) {
		if (count == 2)
			return 0;
		const auto current = address + completed;
		const auto *translated = tlb->translate_tagged(
			current, ::uml::memory::Access::read);
		if (translated == nullptr)
			return 0;
		const auto length = std::min(size - completed,
			page_size - static_cast<std::size_t>(current % page_size));
		pages[count] = translated;
		lengths[count] = length;
		++count;
		completed += length;
	}
	auto *bytes = static_cast<std::byte *>(data);
	completed = 0;
	for (std::size_t index = 0; index < count; ++index) {
		std::memcpy(bytes + completed, pages[index], lengths[index]);
		completed += lengths[index];
	}
	return 1;
}
