// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/memory.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::backend {

class StaticGuestMemory final : public arm64::GuestMemory {
public:
	void set_generation(unsigned long generation) noexcept;
	void prepare(unsigned long generation, std::uint64_t changes) noexcept;

	[[nodiscard]] bool read(
		std::uint64_t address, std::span<std::byte> destination) override;
	[[nodiscard]] bool fetch(
		std::uint64_t address, std::span<std::byte> destination) override;
	[[nodiscard]] bool write(
		std::uint64_t address, std::span<const std::byte> source) override;
	[[nodiscard]] std::byte *atomic_pointer(
		std::uint64_t address, std::size_t size,
		arm64::MemoryAccess access) override;
	[[nodiscard]] bool read_allocation_tag(
		std::uint64_t address, std::uint8_t &tag) override;
	[[nodiscard]] bool write_allocation_tag(
		std::uint64_t address, std::uint8_t tag) override;
	void invalidate_code(std::uint64_t address) override;
	[[nodiscard]] bool validate_write(
		std::uint64_t address, std::size_t size);
	[[nodiscard]] arm64::MemoryFault last_fault() const noexcept override;

private:
	static constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
	static constexpr std::size_t tlb_size = 1U << 13;

	struct TlbEntry final {
		std::uint64_t page{1};
		std::uint64_t generation{};
		std::uint64_t changes{};
		std::array<std::byte *, 3> bases{};
	};

	[[nodiscard]] static constexpr std::size_t index_for(
		std::uint64_t address) noexcept;
	[[nodiscard]] static constexpr std::size_t access_index(
		arm64::MemoryAccess access) noexcept;
	[[nodiscard]] static constexpr darwin_uml_static_access host_access(
		arm64::MemoryAccess access) noexcept;
	[[nodiscard]] std::byte *translate(
		std::uint64_t address, arm64::MemoryAccess access);
	[[nodiscard]] bool copy_from_guest(
		std::uint64_t address, std::span<std::byte> destination,
		arm64::MemoryAccess access);
	bool record_fault(std::uint64_t address, arm64::MemoryAccess access);

	unsigned long generation_{};
	std::uint64_t changes_{};
	std::array<TlbEntry, tlb_size> tlb_{};
	arm64::MemoryFault fault_{};
};

} // namespace uml::backend
