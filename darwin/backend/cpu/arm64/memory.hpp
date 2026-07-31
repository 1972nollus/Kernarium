// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::backend::arm64 {

enum class MemoryAccess {
	read,
	write,
	execute,
};

struct MemoryFault final {
	std::uint64_t address{};
	MemoryAccess access{MemoryAccess::read};
};

class GuestMemory {
public:
	virtual ~GuestMemory() = default;
	[[nodiscard]] virtual bool read(std::uint64_t address,
					std::span<std::byte> destination) = 0;
	[[nodiscard]] virtual bool write(std::uint64_t address,
					 std::span<const std::byte> source) = 0;
	/* Validate an access without changing guest memory. Backends with
	 * protection-aware mappings override this so instructions that perform
	 * allocation-tag checks can preserve the architectural fault priority:
	 * translation/permission first, then the tag check for that access.
	 */
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
	/* Return a directly addressable, naturally contiguous host range for a
	 * scalar atomic access.  Backends that cannot provide one may return null;
	 * the dispatcher then falls back to read()/write() while retaining the
	 * same architectural exclusive-monitor bookkeeping.
	 */
	[[nodiscard]] virtual std::byte *atomic_pointer(
		std::uint64_t, std::size_t, MemoryAccess)
	{
		return nullptr;
	}
	/* Return a stable host identity for reservation bookkeeping without
	 * selecting direct host atomic execution. Backends whose direct atomic
	 * pointer is already stable inherit it; fault-tolerant backends may
	 * override only this identity path and retain read()/write() semantics.
	 */
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
};

} // namespace uml::backend::arm64
