// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/dispatcher.hpp"
#include "memory/static_guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace uml::cpu::arm64::gadgets {

[[nodiscard]] bool verification_enabled() noexcept;

class VerificationMemory final : public arm64::GuestMemory {
public:
	explicit VerificationMemory(
		::uml::memory::StaticGuestMemory &memory) noexcept;
	bool read(std::uint64_t address,
		std::span<std::byte> destination) override;
	bool fetch(std::uint64_t address,
		std::span<std::byte> destination) override;
	bool write(std::uint64_t address,
		std::span<const std::byte> source) override;
	bool validate_access(std::uint64_t address, std::size_t size,
		arm64::MemoryAccess access) override;
	arm64::MemoryFault last_fault() const noexcept override;
	[[nodiscard]] bool matches_guest() const;

private:
	void apply(std::uint64_t address,
		std::span<std::byte> destination) const;

	::uml::memory::StaticGuestMemory &memory_;
	std::unordered_map<std::uint64_t, std::byte> writes_;
};

[[nodiscard]] bool same_cpu_state(
	const arm64::CpuState &expected, const arm64::CpuState &actual,
	std::uint64_t pc);

} // namespace uml::cpu::arm64::gadgets
