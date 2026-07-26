// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::backend::arm64::gadgets {

enum class Access : std::uint32_t {
	read,
	write,
};

struct alignas(32) TlbEntry final {
	std::uint64_t read_page{1};
	std::uint64_t write_page{1};
	std::uintptr_t read_data_minus_address{};
	std::uintptr_t write_data_minus_address{};
};

inline constexpr std::size_t tlb_entry_count = 1U << 13;

/* Public data layout is an assembly ABI. Static assertions in tlb.cpp keep
 * offsets.inc synchronized with this type.
 */
struct alignas(32) Tlb final {
	std::array<TlbEntry, tlb_entry_count> entries{};
	std::uint64_t generation{};
	std::uint64_t changes{};
	std::uint64_t fault_address{};
	Access fault_access{Access::read};
	std::uint32_t reserved{};

	Tlb();
	void prepare(std::uint64_t next_generation, std::uint64_t next_changes);
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

} // namespace uml::backend::arm64::gadgets
