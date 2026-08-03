// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::memory {

enum class Access : std::uint32_t {
	read,
	write,
};

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

	Tlb();
	void prepare(std::uint64_t next_generation, std::uint64_t next_changes);
	void discard(std::uint64_t address);
	[[nodiscard]] std::byte *translate(std::uint64_t address, Access access);
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

