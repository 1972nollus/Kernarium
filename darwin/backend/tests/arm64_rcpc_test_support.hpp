// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace arm64_rcpc_test {

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;

inline constexpr std::uint64_t pc = 0x450'0000;
inline constexpr std::uint64_t base = 0xf0'0400;

[[nodiscard]] inline constexpr std::uint32_t no_offset(unsigned size_code,
	unsigned rn, unsigned rt)
{
	return 0x38bf'c000U | ((size_code & 3U) << 30) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint32_t unscaled(unsigned size_code,
	unsigned operation, int offset, unsigned rn, unsigned rt)
{
	return 0x1900'0000U | ((size_code & 3U) << 30) |
		((operation & 3U) << 22) |
		((static_cast<unsigned>(offset) & 0x1ffU) << 12) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint64_t mask(std::size_t size)
{
	return size == 8 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << (size * 8)) - 1;
}

[[nodiscard]] inline constexpr std::int64_t sign_extend(
	std::uint64_t value, unsigned bits)
{
	const auto shift = 64U - bits;
	return static_cast<std::int64_t>(value << shift) >> shift;
}

[[nodiscard]] inline constexpr std::uint64_t expected_load(
	unsigned size_code, unsigned operation, std::uint64_t raw)
{
	const auto size = std::size_t{1} << size_code;
	raw &= mask(size);
	if (operation >= 2U)
		raw = static_cast<std::uint64_t>(
			sign_extend(raw, static_cast<unsigned>(size * 8)));
	if (size_code != 3U && operation != 2U)
		raw &= 0xffff'ffffU;
	return raw;
}

class alignas(64) Memory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> output) override
	{
		++reads;
		if (fault || !range_valid(address, output.size()))
			return false;
		std::copy_n(pointer(address), output.size(), output.begin());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> input) override
	{
		++writes;
		if (fault || !range_valid(address, input.size()))
			return false;
		std::copy(input.begin(), input.end(), pointer(address));
		return true;
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, arm64::MemoryAccess) override
	{
		++atomic_pointers;
		return direct && !fault && range_valid(address, size) ?
			pointer(address) : nullptr;
	}

	void put(std::uint64_t address, std::uint64_t value, std::size_t size)
	{
		std::memcpy(pointer(address), &value, size);
	}

	[[nodiscard]] std::uint64_t get(std::uint64_t address,
		std::size_t size) const
	{
		std::uint64_t value{};
		std::memcpy(&value, pointer(address), size);
		return value;
	}

	bool direct{true};
	bool fault{};
	unsigned reads{};
	unsigned writes{};
	unsigned atomic_pointers{};

private:
	static constexpr std::uint64_t start = base - 512;

	[[nodiscard]] bool range_valid(std::uint64_t address,
		std::size_t size) const
	{
		return address >= start && address - start <= bytes.size() &&
			size <= bytes.size() - (address - start);
	}

	[[nodiscard]] std::byte *pointer(std::uint64_t address)
	{
		return bytes.data() + (address - start);
	}

	[[nodiscard]] const std::byte *pointer(std::uint64_t address) const
	{
		return bytes.data() + (address - start);
	}

	alignas(64) std::array<std::byte, 1024> bytes{};
};

} // namespace arm64_rcpc_test
