// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace arm64_base_cache_zero_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

inline constexpr std::size_t block_size = 64;
inline constexpr std::size_t block_offset = 128;
inline constexpr std::byte fill{0x5a};

[[nodiscard]] constexpr std::uint32_t instruction(unsigned source)
{
	return 0xd50b'7420U | (source & 31U);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(reg + seed) *
			 0x0101'0101'0101'0101ULL);
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed * 67U + reg * 29U + byte * 17U + 3U);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = seed & 1U;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	return cpu;
}

[[nodiscard]] inline std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE>
filled_page()
{
	std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> page{};
	page.fill(fill);
	return page;
}

[[nodiscard]] inline std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE>
zeroed_page(std::size_t offset)
{
	auto page = filled_page();
	std::fill_n(page.begin() + static_cast<std::ptrdiff_t>(offset),
		block_size, std::byte{});
	return page;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::baseline();
}

} // namespace arm64_base_cache_zero_test
