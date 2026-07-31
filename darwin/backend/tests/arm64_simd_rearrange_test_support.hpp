// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_simd_rearrange_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

struct PermuteRegisters final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

struct TableRegisters final {
	unsigned destination;
	unsigned first;
	unsigned indices;
};

struct PermuteOperation final {
	unsigned opcode;
	std::string_view name;
};

inline constexpr std::array permute_operations{
	PermuteOperation{2U, "FPSIMD-0403 TRN1"},
	PermuteOperation{6U, "FPSIMD-0404 TRN2"},
	PermuteOperation{1U, "FPSIMD-0462 UZP1"},
	PermuteOperation{5U, "FPSIMD-0463 UZP2"},
	PermuteOperation{3U, "FPSIMD-0466 ZIP1"},
	PermuteOperation{7U, "FPSIMD-0467 ZIP2"},
};

[[nodiscard]] constexpr std::string_view table_name(bool extend)
{
	return extend ? "FPSIMD-0402 TBX" : "FPSIMD-0401 TBL";
}

[[nodiscard]] constexpr std::uint32_t permute_instruction(bool q,
	unsigned size, unsigned opcode, PermuteRegisters registers)
{
	return 0x0e00'0800U | (q ? 0x4000'0000U : 0U) |
		((size & 3U) << 22) | ((registers.right & 31U) << 16) |
		((opcode & 7U) << 12) | ((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr bool valid_permute(bool q, unsigned size)
{
	return q || size != 3U;
}

[[nodiscard]] constexpr std::uint32_t table_instruction(bool q, bool extend,
	unsigned count, TableRegisters registers)
{
	return 0x0e00'0000U | (q ? 0x4000'0000U : 0U) |
		((registers.indices & 31U) << 16) |
		(((count - 1U) & 3U) << 13) | (extend ? 0x1000U : 0U) |
		((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed * 67U + reg * 29U + byte * 17U + 3U);
	for (unsigned reg = 0; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(reg + seed) *
			 0x0101'0101'0101'0101ULL);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = seed & 1U;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	return cpu;
}

/*
 * Independent specification oracle. It derives one destination lane at a
 * time from the architectural lane mapping, unlike the implementation's
 * pair-at-a-time copy loop. Both sources are snapshotted before applying the
 * result so destructive aliases exercise architectural read-before-write.
 */
[[nodiscard]] inline arm64::VectorRegister expected_permute_vector(
	const arm64::CpuState &initial, bool q, unsigned size, unsigned opcode,
	PermuteRegisters registers)
{
	const std::size_t vector_size = q ? 16U : 8U;
	const std::size_t element_size = std::size_t{1} << size;
	const std::size_t elements = vector_size / element_size;
	const auto left = initial.vectors[registers.left].bytes;
	const auto right = initial.vectors[registers.right].bytes;
	arm64::VectorRegister result{};
	for (std::size_t lane = 0; lane < elements; ++lane) {
		const std::array<std::byte, 16> *source{};
		std::size_t source_lane{};
		if (opcode == 1U || opcode == 5U) {
			const auto local_lane = lane % (elements / 2U);
			source = lane < elements / 2U ? &left : &right;
			source_lane = local_lane * 2U + (opcode == 5U ? 1U : 0U);
		} else if (opcode == 2U || opcode == 6U) {
			source = (lane & 1U) == 0U ? &left : &right;
			source_lane =
				(lane / 2U) * 2U + (opcode == 6U ? 1U : 0U);
		} else {
			source = (lane & 1U) == 0U ? &left : &right;
			source_lane =
				(opcode == 7U ? elements / 2U : 0U) + lane / 2U;
		}
		std::copy_n(source->begin() + source_lane * element_size,
			element_size, result.bytes.begin() + lane * element_size);
	}
	return result;
}

[[nodiscard]] inline arm64::CpuState expected_permute_state(
	const arm64::CpuState &initial, bool q, unsigned size, unsigned opcode,
	PermuteRegisters registers)
{
	auto expected = initial;
	expected.vectors[registers.destination] =
		expected_permute_vector(initial, q, size, opcode, registers);
	return expected;
}

/*
 * Independent byte-table oracle. First flatten the modulo-32 register list
 * into the architectural 64-byte table, then perform lane lookups. This keeps
 * the oracle distinct from the implementation's per-index register selection.
 */
[[nodiscard]] inline arm64::VectorRegister expected_table_vector(
	const arm64::CpuState &initial, bool q, bool extend, unsigned count,
	TableRegisters registers)
{
	const auto indices = initial.vectors[registers.indices].bytes;
	std::array<std::byte, 64> table{};
	for (unsigned table_register = 0; table_register < count;
	     ++table_register)
		std::copy_n(
			initial.vectors[
				(registers.first + table_register) & 31U].bytes.begin(),
			16, table.begin() + table_register * 16U);
	arm64::VectorRegister result = extend ?
		initial.vectors[registers.destination] :
		arm64::VectorRegister{};
	const unsigned bytes = q ? 16U : 8U;
	for (unsigned output = 0; output < bytes; ++output) {
		const unsigned index = std::to_integer<unsigned>(indices[output]);
		if (index < count * 16U)
			result.bytes[output] = table[index];
	}
	for (unsigned output = bytes; output < 16; ++output)
		result.bytes[output] = std::byte{};
	return result;
}

[[nodiscard]] inline arm64::CpuState expected_table_state(
	const arm64::CpuState &initial, bool q, bool extend, unsigned count,
	TableRegisters registers)
{
	auto expected = initial;
	expected.vectors[registers.destination] =
		expected_table_vector(initial, q, extend, count, registers);
	return expected;
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		uml::backend::ArmFeature::advanced_simd);
}

} // namespace arm64_simd_rearrange_test
