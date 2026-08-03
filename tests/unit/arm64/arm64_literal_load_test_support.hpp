// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_literal_load_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint64_t code_base =
	engine_tests::engine_test_code_base;

enum class ResultKind {
	zero_extend,
	sign_extend,
	vector,
	prefetch,
	reserved,
};

struct Variant final {
	bool vector;
	unsigned operation;
	std::size_t size;
	ResultKind result;
	std::string_view name;
};

/*
 * ISA_A64_xml_A_profile-2026-06, document pages 448, 471, and 1368:
 * V=0 allocates 32-bit zero-extending, 64-bit, and signed-word loads for
 * opc=00..10. V=1 allocates 32-, 64-, and 128-bit vector-register loads for
 * the same values. The vector-register opc=11 quadrant is reserved.
 */
inline constexpr std::array load_variants{
	Variant{false, 0, 4, ResultKind::zero_extend, "ldr-w"},
	Variant{false, 1, 8, ResultKind::zero_extend, "ldr-x"},
	Variant{false, 2, 4, ResultKind::sign_extend, "ldrsw"},
	Variant{true, 0, 4, ResultKind::vector, "ldr-s"},
	Variant{true, 1, 8, ResultKind::vector, "ldr-d"},
	Variant{true, 2, 16, ResultKind::vector, "ldr-q"},
};

inline constexpr Variant prefetch_variant{
	false, 3, 0, ResultKind::prefetch, "prfm-literal"};
inline constexpr Variant reserved_variant{
	true, 3, 0, ResultKind::reserved, "reserved-vector-opc-11"};

[[nodiscard]] constexpr std::uint32_t literal(
	bool vector, unsigned operation, unsigned imm19, unsigned rt)
{
	return 0x1800'0000U | ((operation & 3U) << 30) |
		(static_cast<unsigned>(vector) << 26) |
		((imm19 & 0x7ffffU) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::int64_t displacement(unsigned imm19)
{
	const auto value = static_cast<std::uint64_t>(imm19 & 0x7ffffU) << 2;
	return static_cast<std::int64_t>(value << (64U - 21U)) >>
		(64U - 21U);
}

[[nodiscard]] constexpr std::uint64_t target(
	std::uint64_t pc, unsigned imm19)
{
	return pc + static_cast<std::uint64_t>(displacement(imm19));
}

[[nodiscard]] constexpr unsigned immediate_for_target(
	std::uint64_t pc, std::uint64_t address)
{
	return static_cast<unsigned>(((address - pc) >> 2) & 0x7ffffU);
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			seed ^ (0x8100'0000'1234'0000ULL |
				(static_cast<std::uint64_t>(index) << 9) | index);
	cpu.sp = seed ^ 0x0123'4567'89ab'cdefULL;
	cpu.nzcv = 0xb000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.tls = seed ^ 0xfedc'ba98'7654'3210ULL;
	for (unsigned index = 0; index < cpu.vectors.size(); ++index)
		for (unsigned byte = 0; byte < cpu.vectors[index].bytes.size();
		     ++byte)
			cpu.vectors[index].bytes[byte] = static_cast<std::byte>(
				(seed + index * 29U + byte * 7U) & 0xffU);
	return cpu;
}

inline void fill_memory(std::span<std::byte> image, unsigned salt)
{
	for (std::size_t offset = 0; offset < image.size(); ++offset)
		image[offset] = static_cast<std::byte>(
			0x80U | ((offset * 37U + salt * 19U + 3U) & 0x7fU));
}

[[nodiscard]] inline std::uint64_t decode_little_endian(
	std::span<const std::byte> image, std::size_t offset, std::size_t size)
{
	std::uint64_t value = 0;
	for (std::size_t index = 0; index < size; ++index)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(image[offset + index])) <<
			(index * 8U);
	return value;
}

[[nodiscard]] constexpr std::uint64_t sign_extend_word(
	std::uint64_t value)
{
	return static_cast<std::uint64_t>(
		static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

inline void apply_load(
	arm64::CpuState &expected, const Variant &variant, unsigned destination,
	std::span<const std::byte> image, std::size_t offset)
{
	if (variant.result == ResultKind::vector) {
		expected.vectors[destination].bytes.fill(std::byte{});
		for (std::size_t index = 0; index < variant.size; ++index)
			expected.vectors[destination].bytes[index] =
				image[offset + index];
		return;
	}
	if (destination == 31U)
		return;
	auto value = decode_little_endian(image, offset, variant.size);
	if (variant.result == ResultKind::sign_extend)
		value = sign_extend_word(value);
	else if (variant.size == 4U)
		value &= 0xffff'ffffU;
	expected.registers[destination] = value;
}

[[nodiscard]] inline engine_tests::NativeRequirements native(
	const Variant &variant)
{
	if (variant.vector)
		return engine_tests::NativeRequirements::features(
			::uml::cpu::ArmFeature::floating_point);
	return engine_tests::NativeRequirements::baseline();
}

[[nodiscard]] constexpr std::uint64_t page_base(std::uint64_t address)
{
	return address & ~static_cast<std::uint64_t>(page_size - 1U);
}

} // namespace arm64_literal_load_test
