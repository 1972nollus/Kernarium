// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace uml::runtime {

class NativeAddressSpace final {
public:
	[[nodiscard]] static bool needs_shadow(
		std::uintptr_t address, std::uint64_t offset,
		std::uint64_t length) noexcept;

	[[nodiscard]] bool contains(
		std::uintptr_t address, std::uint64_t length) const;
	int map(std::uintptr_t address, int fd, std::uint64_t offset,
		std::uint64_t length, int protection);
	int protect(std::uintptr_t address, std::uint64_t length,
		    int protection);
	int unmap(std::uintptr_t address, std::uint64_t length);
	int flush(std::uintptr_t address, std::uint64_t length);
	int refresh(std::uintptr_t address, std::uint64_t length);
	int retry(std::uintptr_t address, bool execute, bool write) const;

private:
	struct Mapping final {
		int fd;
		std::uint64_t offset;
		int protection;
		std::array<std::byte, 4096> original{};
		bool loaded{};
	};

	int flush_page_locked(std::uintptr_t page);
	int flush_slot_locked(std::uintptr_t slot);
	int load_page_locked(std::uintptr_t page);
	int materialize_slot_locked(std::uintptr_t slot);
	int update_slot_protection_locked(std::uintptr_t slot);
	[[nodiscard]] bool contains_locked(
		std::uintptr_t address, std::uint64_t length) const;

	mutable std::mutex mutex_;
	std::unordered_map<std::uintptr_t, Mapping> mappings_;
	std::unordered_set<std::uintptr_t> materialized_slots_;
};

NativeAddressSpace &native_address_space();

} // namespace uml::runtime
