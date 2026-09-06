// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "memory/guest_address_space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::memory {

enum class Access : std::uint32_t {
	read,
	write,
	execute,
};

[[nodiscard]] constexpr Access translated_access(
	MemoryAccess access) noexcept
{
	switch (access) {
	case MemoryAccess::read:
		return Access::read;
	case MemoryAccess::write:
		return Access::write;
	case MemoryAccess::execute:
		return Access::execute;
	}
	return Access::read;
}

[[nodiscard]] constexpr MemoryAccess guest_access(Access access) noexcept
{
	switch (access) {
	case Access::read:
		return MemoryAccess::read;
	case Access::write:
		return MemoryAccess::write;
	case Access::execute:
		return MemoryAccess::execute;
	}
	return MemoryAccess::read;
}

struct alignas(32) TlbEntry final {
	std::uint64_t read_page{1};
	std::uintptr_t read_data_minus_address{};
	std::uint64_t write_page{1};
	std::uintptr_t write_data_minus_address{};
};

inline constexpr std::size_t tlb_index_bits =
	DARWIN_UML_GUEST_PAGE_SIZE == 16384 ? 14U : 13U;
inline constexpr std::size_t tlb_set_count = 1U << tlb_index_bits;
inline constexpr std::size_t tlb_way_count =
	DARWIN_UML_GUEST_PAGE_SIZE == 16384 ? 2U : 1U;
inline constexpr std::size_t tlb_entry_count =
	tlb_set_count * tlb_way_count;

/*
 * This layout is shared by the gadget and JIT engines and their assembly
 * helpers. Keep it in the memory layer so either engine can consume the TLB
 * contract without depending on the other engine's implementation headers.
 */
struct alignas(32) Tlb final {
	std::array<TlbEntry, tlb_entry_count> entries{};
	std::uint64_t generation{};
	std::uint64_t changes{};
	std::uint64_t fault_address{};
	Access fault_access{Access::read};
	std::uint32_t reserved{};
	std::array<std::uint8_t, tlb_set_count> replacement_way{};
	/* Whether the address space owning the current entries ignores the top
	 * address byte (TBI or MTE). Set by the executor that fills the TLB
	 * from its guest state; consumers that never present tagged addresses
	 * leave it false. */
	bool tagged_addresses{};

	Tlb();
	void prepare(std::uint64_t next_generation, std::uint64_t next_changes);
	void discard(std::uint64_t address);
	[[nodiscard]] std::byte *translate(std::uint64_t address, Access access);
	/* Translate an address that may still carry a logical tag. The entry is
	 * keyed by the address as presented, so a later lookup with the same
	 * tag hits without stripping it; a fault reports the untagged address.
	 * Invalidation ignores the tag byte of every key, so tagged entries
	 * follow their page's mapping changes. */
	[[nodiscard]] std::byte *translate_tagged(
		std::uint64_t address, Access access);
	[[nodiscard]] bool read_allocation_tag(
		std::uint64_t address, std::uint8_t &tag);
	[[nodiscard]] bool write_allocation_tag(
		std::uint64_t address, std::uint8_t tag);

private:
	void flush();
	void invalidate(std::uint64_t address, std::uint64_t length);
	void reconcile_changes(
		std::uint64_t next_generation, std::uint64_t next_changes);
};

} // namespace uml::memory
