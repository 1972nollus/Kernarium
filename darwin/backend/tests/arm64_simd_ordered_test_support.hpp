// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace arm64_simd_ordered_test {

namespace common = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

using common::require;

inline constexpr std::uint64_t pc = 0x2d0'0000;
inline constexpr std::uint64_t base = 0xb8'0000;

struct Variant final {
	unsigned size_code;
	unsigned opc;
	std::size_t size;
	bool load;
};

inline constexpr std::array variants{
	Variant{0, 0, 1, false}, Variant{0, 1, 1, true},
	Variant{1, 0, 2, false}, Variant{1, 1, 2, true},
	Variant{2, 0, 4, false}, Variant{2, 1, 4, true},
	Variant{3, 0, 8, false}, Variant{3, 1, 8, true},
	Variant{0, 2, 16, false}, Variant{0, 3, 16, true},
};

[[nodiscard]] constexpr std::uint32_t unscaled(unsigned size_code,
	unsigned opc, unsigned imm9, unsigned rn, unsigned rt)
{
	return 0x1d00'0800U | ((size_code & 3U) << 30) |
		((opc & 3U) << 22) | ((imm9 & 0x1ffU) << 12) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t lane(bool load, bool upper,
	unsigned rn, unsigned rt)
{
	return 0x0d01'8400U | (static_cast<unsigned>(upper) << 30) |
		(static_cast<unsigned>(load) << 22) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline arm64::VectorRegister pattern(unsigned seed = 0)
{
	arm64::VectorRegister result;
	for (unsigned index = 0; index < result.bytes.size(); ++index)
		result.bytes[index] = static_cast<std::byte>(
			0xe3U - index * 13U + seed * 29U);
	return result;
}

inline void require_loaded(const arm64::VectorRegister &actual,
	const common::ProbeMemory &memory, std::size_t size)
{
	for (std::size_t index = 0; index < actual.bytes.size(); ++index)
		require(actual.bytes[index] ==
			(index < size ? memory.payload[index] : std::byte{}));
}

inline void require_state_unchanged(const arm64::CpuState &actual,
	const arm64::CpuState &before)
{
	require(actual.registers == before.registers);
	require(actual.sp == before.sp);
	for (unsigned index = 0; index < actual.vectors.size(); ++index)
		require(actual.vectors[index].bytes == before.vectors[index].bytes);
	require(actual.pc == before.pc);
	require(actual.nzcv == before.nzcv);
	require(actual.fpcr == before.fpcr);
	require(actual.fpsr == before.fpsr);
}

inline void require_vectors_equal(const arm64::CpuState &actual,
	const arm64::CpuState &before)
{
	for (unsigned index = 0; index < actual.vectors.size(); ++index)
		require(actual.vectors[index].bytes == before.vectors[index].bytes);
}

inline void initialize(arm64::CpuState &cpu)
{
	common::fill_registers(cpu);
	cpu.pc = pc;
	for (unsigned index = 0; index < cpu.vectors.size(); ++index)
		cpu.vectors[index] = pattern(index);
}

} // namespace arm64_simd_ordered_test
