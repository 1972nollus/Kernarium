// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::memory {

enum class MemoryAccess {
	read,
	write,
	execute,
};

struct MemoryFault final {
	std::uint64_t address{};
	MemoryAccess access{MemoryAccess::read};
};

class GuestAddressSpace {
public:
	virtual ~GuestAddressSpace() = default;
	[[nodiscard]] virtual bool read(std::uint64_t address,
					std::span<std::byte> destination) = 0;
	[[nodiscard]] virtual bool write(std::uint64_t address,
					 std::span<const std::byte> source) = 0;
	/* Validate an access without changing guest memory. Emulators with
	 * protection-aware mappings override this so instructions that perform
	 * allocation-tag checks preserve architectural fault priority. */
	[[nodiscard]] virtual bool validate_access(
		std::uint64_t, std::size_t, MemoryAccess)
	{
		return true;
	}
	[[nodiscard]] virtual bool fetch(std::uint64_t address,
					 std::span<std::byte> destination)
	{
		return read(address, destination);
	}
	[[nodiscard]] virtual std::byte *atomic_pointer(
		std::uint64_t, std::size_t, MemoryAccess)
	{
		return nullptr;
	}
	[[nodiscard]] virtual std::byte *reservation_pointer(
		std::uint64_t address, std::size_t size, MemoryAccess access)
	{
		return atomic_pointer(address, size, access);
	}
	[[nodiscard]] virtual bool read_allocation_tag(
		std::uint64_t, std::uint8_t &)
	{
		return false;
	}
	[[nodiscard]] virtual bool write_allocation_tag(
		std::uint64_t, std::uint8_t)
	{
		return false;
	}
	virtual void invalidate_code(std::uint64_t) {}
	[[nodiscard]] virtual MemoryFault last_fault() const noexcept
	{
		return {};
	}
	[[nodiscard]] virtual bool has_fault() const noexcept
	{
		return false;
	}
};

} // namespace uml::memory
