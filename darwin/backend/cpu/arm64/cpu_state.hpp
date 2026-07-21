// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::backend::arm64 {

struct alignas(16) VectorRegister final {
	std::array<std::byte, 16> bytes{};
};

struct CpuState final {
	std::array<std::uint64_t, 31> registers{};
	std::uint64_t sp{};
	std::uint64_t pc{};
	std::uint32_t nzcv{};
	std::array<VectorRegister, 32> vectors{};
	std::uint32_t fpcr{};
	std::uint32_t fpsr{};
	std::uint64_t tls{};
	std::uint64_t exclusive_address{~std::uint64_t{0}};
	std::uintptr_t exclusive_key{};
	std::uint64_t exclusive_value{};
	std::uint64_t exclusive_version{};
	std::uint8_t exclusive_size{};

	[[nodiscard]] std::uint64_t read_register(unsigned index,
					     bool stack_pointer) const noexcept
	{
		if (index < registers.size())
			return registers[index];
		return stack_pointer ? sp : 0;
	}

	void write_register(unsigned index, std::uint64_t value,
			    bool stack_pointer) noexcept
	{
		if (index < registers.size())
			registers[index] = value;
		else if (stack_pointer)
			sp = value;
	}
};

} // namespace uml::backend::arm64
