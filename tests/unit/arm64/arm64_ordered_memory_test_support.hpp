// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

namespace arm64_ordered_memory_test {

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;

inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint64_t page_base = engine::engine_test_data_base;
inline constexpr std::uint64_t address =
	page_base + page_size / 2U;
inline constexpr std::uint64_t unmapped_address =
	page_base + 32U * page_size;
inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

[[nodiscard]] constexpr std::uint32_t scalar_exclusive(
	unsigned size_code, bool load, unsigned rn, unsigned rt,
	unsigned status = 31U)
{
	return 0x0800'0000U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(load) << 22) |
		((status & 31U) << 16) | (31U << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

enum class ScalarClass {
	ordered,
	rcpc_no_offset,
	rcpc_unscaled,
	writeback,
};

struct ScalarVariant final {
	std::string_view row;
	std::string_view name;
	ScalarClass instruction_class;
	unsigned size_code;
	unsigned operation;
	bool load;
	::uml::cpu::ArmFeature feature;
};

/*
 * ISA_A64_xml_A_profile-2026-06, pages 395-411, 432-438, 740-751.
 * Each entry below is an independently allocated assembler variant. Rows
 * shared by multiple widths or result widths deliberately occur more than
 * once so their complete encoding allocation is exercised.
 */
inline constexpr std::array scalar_variants{
	ScalarVariant{"BASE-0407", "stllrb", ScalarClass::ordered, 0, 0, false,
		::uml::cpu::ArmFeature::lor},
	ScalarVariant{"BASE-0216", "ldlarb", ScalarClass::ordered, 0, 0, true,
		::uml::cpu::ArmFeature::lor},
	ScalarVariant{"BASE-0411", "stlrb", ScalarClass::ordered, 0, 1, false,
		::uml::cpu::ArmFeature::inventory_count},
	ScalarVariant{"BASE-0198", "ldarb", ScalarClass::ordered, 0, 1, true,
		::uml::cpu::ArmFeature::inventory_count},
	ScalarVariant{"BASE-0408", "stllrh", ScalarClass::ordered, 1, 0, false,
		::uml::cpu::ArmFeature::lor},
	ScalarVariant{"BASE-0217", "ldlarh", ScalarClass::ordered, 1, 0, true,
		::uml::cpu::ArmFeature::lor},
	ScalarVariant{"BASE-0412", "stlrh", ScalarClass::ordered, 1, 1, false,
		::uml::cpu::ArmFeature::inventory_count},
	ScalarVariant{"BASE-0199", "ldarh", ScalarClass::ordered, 1, 1, true,
		::uml::cpu::ArmFeature::inventory_count},
	ScalarVariant{"BASE-0406", "stllr-w", ScalarClass::ordered, 2, 0, false,
		::uml::cpu::ArmFeature::lor},
	ScalarVariant{"BASE-0215", "ldlar-w", ScalarClass::ordered, 2, 0, true,
		::uml::cpu::ArmFeature::lor},
	ScalarVariant{"BASE-0410", "stlr-w", ScalarClass::ordered, 2, 1, false,
		::uml::cpu::ArmFeature::inventory_count},
	ScalarVariant{"BASE-0197", "ldar-w", ScalarClass::ordered, 2, 1, true,
		::uml::cpu::ArmFeature::inventory_count},
	ScalarVariant{"BASE-0406", "stllr-x", ScalarClass::ordered, 3, 0, false,
		::uml::cpu::ArmFeature::lor},
	ScalarVariant{"BASE-0215", "ldlar-x", ScalarClass::ordered, 3, 0, true,
		::uml::cpu::ArmFeature::lor},
	ScalarVariant{"BASE-0410", "stlr-x", ScalarClass::ordered, 3, 1, false,
		::uml::cpu::ArmFeature::inventory_count},
	ScalarVariant{"BASE-0197", "ldar-x", ScalarClass::ordered, 3, 1, true,
		::uml::cpu::ArmFeature::inventory_count},

	ScalarVariant{"BASE-0189", "ldaprb", ScalarClass::rcpc_no_offset, 0, 1,
		true, ::uml::cpu::ArmFeature::lrcpc},
	ScalarVariant{"BASE-0190", "ldaprh", ScalarClass::rcpc_no_offset, 1, 1,
		true, ::uml::cpu::ArmFeature::lrcpc},
	ScalarVariant{"BASE-0188", "ldapr-w", ScalarClass::rcpc_no_offset, 2, 1,
		true, ::uml::cpu::ArmFeature::lrcpc},
	ScalarVariant{"BASE-0188", "ldapr-x", ScalarClass::rcpc_no_offset, 3, 1,
		true, ::uml::cpu::ArmFeature::lrcpc},

	ScalarVariant{"BASE-0415", "stlurb", ScalarClass::rcpc_unscaled, 0, 0,
		false, ::uml::cpu::ArmFeature::lrcpc2},
	ScalarVariant{"BASE-0414", "stlur-w", ScalarClass::rcpc_unscaled, 2, 0,
		false, ::uml::cpu::ArmFeature::lrcpc2},
	ScalarVariant{"BASE-0416", "stlurh", ScalarClass::rcpc_unscaled, 1, 0,
		false, ::uml::cpu::ArmFeature::lrcpc2},
	ScalarVariant{"BASE-0414", "stlur-x", ScalarClass::rcpc_unscaled, 3, 0,
		false, ::uml::cpu::ArmFeature::lrcpc2},
	ScalarVariant{"BASE-0192", "ldapurb", ScalarClass::rcpc_unscaled, 0, 1,
		true, ::uml::cpu::ArmFeature::lrcpc2},
	ScalarVariant{"BASE-0193", "ldapurh", ScalarClass::rcpc_unscaled, 1, 1,
		true, ::uml::cpu::ArmFeature::lrcpc2},
	ScalarVariant{"BASE-0191", "ldapur-w", ScalarClass::rcpc_unscaled, 2, 1,
		true, ::uml::cpu::ArmFeature::lrcpc2},
	ScalarVariant{"BASE-0191", "ldapur-x", ScalarClass::rcpc_unscaled, 3, 1,
		true, ::uml::cpu::ArmFeature::lrcpc2},
	ScalarVariant{"BASE-0194", "ldapursb-x", ScalarClass::rcpc_unscaled, 0, 2,
		true, ::uml::cpu::ArmFeature::lrcpc2},
	ScalarVariant{"BASE-0195", "ldapursh-x", ScalarClass::rcpc_unscaled, 1, 2,
		true, ::uml::cpu::ArmFeature::lrcpc2},
	ScalarVariant{"BASE-0196", "ldapursw-x", ScalarClass::rcpc_unscaled, 2, 2,
		true, ::uml::cpu::ArmFeature::lrcpc2},
	ScalarVariant{"BASE-0194", "ldapursb-w", ScalarClass::rcpc_unscaled, 0, 3,
		true, ::uml::cpu::ArmFeature::lrcpc2},
	ScalarVariant{"BASE-0195", "ldapursh-w", ScalarClass::rcpc_unscaled, 1, 3,
		true, ::uml::cpu::ArmFeature::lrcpc2},

	ScalarVariant{"BASE-0410", "stlr-w-pre", ScalarClass::writeback, 2, 0,
		false, ::uml::cpu::ArmFeature::lrcpc3},
	ScalarVariant{"BASE-0188", "ldapr-w-post", ScalarClass::writeback, 2, 1,
		true, ::uml::cpu::ArmFeature::lrcpc3},
	ScalarVariant{"BASE-0410", "stlr-x-pre", ScalarClass::writeback, 3, 0,
		false, ::uml::cpu::ArmFeature::lrcpc3},
	ScalarVariant{"BASE-0188", "ldapr-x-post", ScalarClass::writeback, 3, 1,
		true, ::uml::cpu::ArmFeature::lrcpc3},
};

enum class PairKind {
	stilp,
	ldiapp,
	stlp,
	ldap,
	ldapp,
};

struct PairVariant final {
	std::string_view row;
	std::string_view name;
	PairKind kind;
	unsigned size_code;
	bool writeback;
	bool load;
	::uml::cpu::ArmFeature feature;
};

inline constexpr std::array pair_variants{
	PairVariant{"BASE-0405", "stilp-w-pre", PairKind::stilp, 2, true, false,
		::uml::cpu::ArmFeature::lrcpc3},
	PairVariant{"BASE-0405", "stilp-w", PairKind::stilp, 2, false, false,
		::uml::cpu::ArmFeature::lrcpc3},
	PairVariant{"BASE-0405", "stilp-x-pre", PairKind::stilp, 3, true, false,
		::uml::cpu::ArmFeature::lrcpc3},
	PairVariant{"BASE-0405", "stilp-x", PairKind::stilp, 3, false, false,
		::uml::cpu::ArmFeature::lrcpc3},
	PairVariant{"BASE-0214", "ldiapp-w-post", PairKind::ldiapp, 2, true, true,
		::uml::cpu::ArmFeature::lrcpc3},
	PairVariant{"BASE-0214", "ldiapp-w", PairKind::ldiapp, 2, false, true,
		::uml::cpu::ArmFeature::lrcpc3},
	PairVariant{"BASE-0214", "ldiapp-x-post", PairKind::ldiapp, 3, true, true,
		::uml::cpu::ArmFeature::lrcpc3},
	PairVariant{"BASE-0214", "ldiapp-x", PairKind::ldiapp, 3, false, true,
		::uml::cpu::ArmFeature::lrcpc3},
	PairVariant{"BASE-0409", "BASE-0409 STLP", PairKind::stlp, 3, false, false,
		::uml::cpu::ArmFeature::lscp},
	PairVariant{"BASE-0186", "BASE-0186 LDAP", PairKind::ldap, 3, false, true,
		::uml::cpu::ArmFeature::lscp},
	PairVariant{"BASE-0187", "BASE-0187 LDAPP", PairKind::ldapp, 3, false, true,
		::uml::cpu::ArmFeature::lscp},
};

enum class SimdKind {
	unscaled,
	lane,
};

struct SimdVariant final {
	std::string_view row;
	std::string_view name;
	SimdKind kind;
	unsigned size_code;
	unsigned operation;
	std::size_t size;
	bool load;
	bool upper;
};

inline constexpr std::array simd_unscaled_variants{
	SimdVariant{"FPSIMD-0388", "stlur-b", SimdKind::unscaled, 0, 0, 1,
		false, false},
	SimdVariant{"FPSIMD-0221", "ldapur-b", SimdKind::unscaled, 0, 1, 1,
		true, false},
	SimdVariant{"FPSIMD-0388", "stlur-h", SimdKind::unscaled, 1, 0, 2,
		false, false},
	SimdVariant{"FPSIMD-0221", "ldapur-h", SimdKind::unscaled, 1, 1, 2,
		true, false},
	SimdVariant{"FPSIMD-0388", "stlur-s", SimdKind::unscaled, 2, 0, 4,
		false, false},
	SimdVariant{"FPSIMD-0221", "ldapur-s", SimdKind::unscaled, 2, 1, 4,
		true, false},
	SimdVariant{"FPSIMD-0388", "stlur-d", SimdKind::unscaled, 3, 0, 8,
		false, false},
	SimdVariant{"FPSIMD-0221", "ldapur-d", SimdKind::unscaled, 3, 1, 8,
		true, false},
	SimdVariant{"FPSIMD-0388", "stlur-q", SimdKind::unscaled, 0, 2, 16,
		false, false},
	SimdVariant{"FPSIMD-0221", "ldapur-q", SimdKind::unscaled, 0, 3, 16,
		true, false},
};

inline constexpr std::array simd_lane_variants{
	SimdVariant{"FPSIMD-0387", "stl1-d0", SimdKind::lane, 0, 0, 8,
		false, false},
	SimdVariant{"FPSIMD-0387", "stl1-d1", SimdKind::lane, 0, 0, 8,
		false, true},
	SimdVariant{"FPSIMD-0220", "ldap1-d0", SimdKind::lane, 0, 0, 8,
		true, false},
	SimdVariant{"FPSIMD-0220", "ldap1-d1", SimdKind::lane, 0, 0, 8,
		true, true},
};

[[nodiscard]] constexpr std::uint32_t scalar_instruction(
	const ScalarVariant &variant, int offset, unsigned rn, unsigned rt)
{
	switch (variant.instruction_class) {
	case ScalarClass::ordered:
		return 0x089f'7c00U |
			((variant.size_code & 3U) << 30) |
			(static_cast<unsigned>(variant.load) << 22) |
			((variant.operation & 1U) << 15) |
			((rn & 31U) << 5) | (rt & 31U);
	case ScalarClass::rcpc_no_offset:
		return 0x38bf'c000U |
			((variant.size_code & 3U) << 30) |
			((rn & 31U) << 5) | (rt & 31U);
	case ScalarClass::rcpc_unscaled:
		return 0x1900'0000U |
			((variant.size_code & 3U) << 30) |
			((variant.operation & 3U) << 22) |
			((static_cast<unsigned>(offset) & 0x1ffU) << 12) |
			((rn & 31U) << 5) | (rt & 31U);
	case ScalarClass::writeback:
		return 0x1980'0800U |
			((variant.size_code & 3U) << 30) |
			(static_cast<unsigned>(variant.load) << 22) |
			((rn & 31U) << 5) | (rt & 31U);
	}
	return 0;
}

[[nodiscard]] constexpr std::uint32_t pair_instruction(
	const PairVariant &variant, unsigned rn, unsigned rt, unsigned rt2)
{
	std::uint32_t fixed = 0;
	switch (variant.kind) {
	case PairKind::stilp:
		fixed = 0x1900'0800U |
			((variant.size_code & 3U) << 30) |
			(static_cast<unsigned>(!variant.writeback) << 12);
		break;
	case PairKind::ldiapp:
		fixed = 0x1940'0800U |
			((variant.size_code & 3U) << 30) |
			(static_cast<unsigned>(!variant.writeback) << 12);
		break;
	case PairKind::stlp: fixed = 0xd900'5800U; break;
	case PairKind::ldap: fixed = 0xd940'5800U; break;
	case PairKind::ldapp: fixed = 0xd940'7800U; break;
	}
	return fixed | ((rt2 & 31U) << 16) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t simd_instruction(
	const SimdVariant &variant, int offset, unsigned rn, unsigned rt)
{
	if (variant.kind == SimdKind::lane)
		return 0x0d01'8400U |
			(static_cast<unsigned>(variant.upper) << 30) |
			(static_cast<unsigned>(variant.load) << 22) |
			((rn & 31U) << 5) | (rt & 31U);
	return 0x1d00'0800U |
		((variant.size_code & 3U) << 30) |
		((variant.operation & 3U) << 22) |
		((static_cast<unsigned>(offset) & 0x1ffU) << 12) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline engine::NativeRequirements native(
	const ScalarVariant &variant)
{
	if (variant.feature == ::uml::cpu::ArmFeature::inventory_count)
		return engine::NativeRequirements::baseline();
	return engine::NativeRequirements::features(variant.feature);
}

[[nodiscard]] inline engine::NativeRequirements native(
	const PairVariant &variant)
{
	return engine::NativeRequirements::features(variant.feature);
}

[[nodiscard]] inline engine::NativeRequirements native(
	const SimdVariant &variant)
{
	if (variant.kind == SimdKind::lane)
		return engine::NativeRequirements::features({
			::uml::cpu::ArmFeature::advanced_simd,
			::uml::cpu::ArmFeature::lrcpc3,
		});
	return engine::NativeRequirements::features({
		::uml::cpu::ArmFeature::floating_point,
		::uml::cpu::ArmFeature::lrcpc3,
	});
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			seed ^ (0x8135'79bd'2468'0000ULL |
				(static_cast<std::uint64_t>(index) << 10) | index);
	cpu.sp = seed ^ 0xfedc'ba98'7654'3210ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'001fU;
	cpu.tls = seed ^ 0x0123'4567'89ab'cdefULL;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				(seed + reg * 29U + byte * 17U + 3U) & 0xffU);
	return cpu;
}

inline void fill_page(std::span<std::byte> bytes, unsigned seed)
{
	for (std::size_t index = 0; index < bytes.size(); ++index)
		bytes[index] = static_cast<std::byte>(
			(seed * 37U + index * 19U + (index >> 4) * 11U) & 0xffU);
}

[[nodiscard]] constexpr std::uint64_t size_mask(std::size_t size)
{
	return size == 8U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << (size * 8U)) - 1U;
}

[[nodiscard]] inline std::uint64_t load_integer(
	std::span<const std::byte> bytes, std::size_t offset, std::size_t size)
{
	std::uint64_t value = 0;
	for (std::size_t index = 0; index < size; ++index)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(bytes[offset + index])) <<
			(index * 8U);
	return value;
}

inline void store_integer(std::span<std::byte> bytes, std::size_t offset,
	std::size_t size, std::uint64_t value)
{
	for (std::size_t index = 0; index < size; ++index)
		bytes[offset + index] = static_cast<std::byte>(
			(value >> (index * 8U)) & 0xffU);
}

[[nodiscard]] constexpr std::uint64_t scalar_load_result(
	const ScalarVariant &variant, std::uint64_t raw)
{
	const auto bits = 8U << variant.size_code;
	raw &= size_mask(std::size_t{1} << variant.size_code);
	if (variant.instruction_class == ScalarClass::rcpc_unscaled &&
	    variant.operation >= 2U) {
		const auto shift = 64U - bits;
		raw = static_cast<std::uint64_t>(
			static_cast<std::int64_t>(raw << shift) >> shift);
	}
	const bool signed_unscaled =
		variant.instruction_class == ScalarClass::rcpc_unscaled &&
		variant.operation >= 2U;
	const bool result_32 =
		signed_unscaled ? variant.operation == 3U :
			variant.size_code < 3U;
	return result_32 ? raw & 0xffff'ffffU : raw;
}

[[nodiscard]] constexpr std::uint64_t effective_scalar_address(
	const ScalarVariant &variant, std::uint64_t base, int offset)
{
	if (variant.instruction_class == ScalarClass::rcpc_unscaled)
		return base + static_cast<std::uint64_t>(
			static_cast<std::int64_t>(offset));
	if (variant.instruction_class == ScalarClass::writeback && !variant.load)
		return base - (std::uint64_t{1} << variant.size_code);
	return base;
}

inline void set_base(arm64::CpuState &cpu, unsigned rn, std::uint64_t value)
{
	if (rn == 31U)
		cpu.sp = value;
	else
		cpu.registers[rn] = value;
}

[[nodiscard]] inline std::uint64_t read_base(
	const arm64::CpuState &cpu, unsigned rn)
{
	return rn == 31U ? cpu.sp : cpu.registers[rn];
}

inline void write_base(arm64::CpuState &cpu, unsigned rn, std::uint64_t value)
{
	if (rn == 31U)
		cpu.sp = value;
	else
		cpu.registers[rn] = value;
}

inline void write_gpr(arm64::CpuState &cpu, unsigned rt,
	std::uint64_t value)
{
	if (rt != 31U)
		cpu.registers[rt] = value;
}

[[nodiscard]] inline std::uint64_t read_gpr(
	const arm64::CpuState &cpu, unsigned rt)
{
	return rt == 31U ? 0U : cpu.registers[rt];
}

[[nodiscard]] inline std::uint64_t scalar_base_for_target(
	const ScalarVariant &variant, std::uint64_t target, int offset)
{
	if (variant.instruction_class == ScalarClass::rcpc_unscaled)
		return target - static_cast<std::uint64_t>(
			static_cast<std::int64_t>(offset));
	if (variant.instruction_class == ScalarClass::writeback && !variant.load)
		return target + (std::uint64_t{1} << variant.size_code);
	return target;
}

inline void describe(char *output, std::size_t size, std::string_view name,
	std::string_view suffix, unsigned rn, unsigned rt)
{
	std::snprintf(output, size, "%.*s %.*s rn=%u rt=%u",
		static_cast<int>(name.size()), name.data(),
		static_cast<int>(suffix.size()), suffix.data(), rn, rt);
}

inline void validate_failed_exclusive(
	const arm64::CpuState &, const arm64::CpuState &expected,
	const arm64::CpuState &actual, std::string_view case_name,
	std::string_view engine_name, std::uint32_t instruction)
{
	auto normalized = actual;
	/*
	 * A successful exclusive load records a mapping key and reservation
	 * generation that are deliberately opaque to architectural tests. The
	 * failed store-exclusive must still clear the architecturally relevant
	 * monitor identity and size.
	 */
	normalized.exclusive_value = expected.exclusive_value;
	normalized.exclusive_value2 = expected.exclusive_value2;
	normalized.exclusive_version = expected.exclusive_version;
	engine::require_architecture_equal(expected, normalized, case_name,
		engine_name, instruction);
}

} // namespace arm64_ordered_memory_test
