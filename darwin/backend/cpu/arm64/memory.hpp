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
	virtual void invalidate_code(std::uint64_t) {}
	[[nodiscard]] virtual MemoryFault last_fault() const noexcept
	{
		return {};
	}
};

} // namespace uml::backend::arm64
