// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>
#include <string_view>

namespace arm64_pauth_test {

namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace test = uml::emulator::tests;

inline constexpr std::uint64_t pc = test::engine_test_code_base;
inline constexpr std::uint64_t data_base = test::engine_test_data_base;
inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
using Page = std::array<std::byte, page_size>;
/* Base pointer authentication is implemented from guest-owned software
 * state in all three engines and therefore has no native-host requirement. */
inline const auto native_pauth = test::NativeRequirements::baseline();
inline const auto native_pauth_lr =
	test::NativeRequirements::features(::uml::cpu::ArmFeature::pauth_lr);

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

class CaseName final {
public:
	template<typename... Arguments>
	explicit CaseName(const char *format, Arguments... arguments)
	{
		const int length = std::snprintf(
			storage_.data(), storage_.size(), format, arguments...);
		require(length >= 0 &&
			static_cast<std::size_t>(length) < storage_.size());
		length_ = static_cast<std::size_t>(length);
	}

	[[nodiscard]] operator std::string_view() const noexcept
	{
		return {storage_.data(), length_};
	}

private:
	std::array<char, 192> storage_{};
	std::size_t length_{};
};

[[nodiscard]] constexpr std::uint32_t data_processing(
	unsigned operation, unsigned destination, unsigned modifier = 31) noexcept
{
	return 0xdac1'0000U | ((operation & 15U) << 10) |
		((modifier & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t hint(unsigned immediate) noexcept
{
	return 0xd503'201fU | ((immediate & 0x7fU) << 5);
}

[[nodiscard]] constexpr std::uint32_t xpac(
	bool data, unsigned destination, unsigned reserved_source = 31) noexcept
{
	return 0xdac1'4000U | (static_cast<unsigned>(data) << 10) |
		((reserved_source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t pacga(
	unsigned destination, unsigned source, unsigned modifier) noexcept
{
	return 0x9ac0'3000U | ((modifier & 31U) << 16) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t authenticated_branch(
	bool link, bool key_b, bool zero_modifier,
	unsigned target, unsigned modifier = 31) noexcept
{
	return (zero_modifier ? 0xd61f'0800U : 0xd71f'0800U) |
		(static_cast<unsigned>(link) << 21) |
		(static_cast<unsigned>(key_b) << 10) |
		((target & 31U) << 5) | (modifier & 31U);
}

[[nodiscard]] constexpr std::uint32_t authenticated_return(bool key_b) noexcept
{
	return key_b ? 0xd65f'0fffU : 0xd65f'0bffU;
}

[[nodiscard]] constexpr std::uint32_t authenticated_load(
	bool key_b, bool writeback, unsigned encoded_offset,
	unsigned base, unsigned destination) noexcept
{
	return 0xf820'0400U | (static_cast<unsigned>(key_b) << 23) |
		(((encoded_offset >> 9) & 1U) << 22) |
		((encoded_offset & 0x1ffU) << 12) |
		(static_cast<unsigned>(writeback) << 11) |
		((base & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint64_t strip(
	std::uint64_t source, bool data) noexcept
{
	const bool upper = (source & (std::uint64_t{1} << 55)) != 0;
	if (data && !upper)
		return source & 0xff00'7fff'ffff'ffffULL;
	return (source & 0x0000'7fff'ffff'ffffULL) |
		(upper ? 0xffff'8000'0000'0000ULL : 0);
}

[[nodiscard]] constexpr std::uint64_t signing_original(
	std::uint64_t source, bool data) noexcept
{
	const bool upper = (source & (std::uint64_t{1} << 55)) != 0;
	if (data && !upper)
		return source & 0xff00'7fff'ffff'ffffULL;
	return (source & 0x0000'7fff'ffff'ffffULL) |
		((source >> 63) != 0 ? 0xffff'8000'0000'0000ULL : 0);
}

[[nodiscard]] inline arm64::CpuState initial_state(unsigned salt = 0)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			0x1020'3040'5060'7000ULL ^
			(static_cast<std::uint64_t>(index + 1) *
				0x0101'0101'0101'0101ULL) ^
			(static_cast<std::uint64_t>(salt) << (index % 13));
	cpu.sp = 0x2468'ace0'1357'9bd0ULL +
		static_cast<std::uint64_t>(salt) * 16;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0010U;
	cpu.fpmr = 0x1020'3040'5060'7080ULL;
	cpu.tls = 0x0123'4567'89ab'cdefULL;
	cpu.set_mte_configuration(true, 0x8001U);
	cpu.set_mte_generator(
		false, 0xace1U, 7, 0xfeed'face'0123'4567ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0xa1b2'c3d4U;
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xa;
	cpu.exclusive_address = 0x7654'3000ULL;
	cpu.exclusive_key = 0x1234U;
	cpu.exclusive_value = 0xa5a5'a5a5'a5a5'a5a5ULL;
	cpu.exclusive_value2 = 0x5a5a'5a5a'5a5a'5a5aULL;
	cpu.exclusive_version = 19;
	cpu.exclusive_size = 16;
	cpu.gcs_pointer = data_base + 0x1000;
	cpu.gcs_implemented = false;
	cpu.gcs_push_enabled = false;
	cpu.gcs_store_enabled = false;
	for (unsigned vector = 0; vector < cpu.vectors.size(); ++vector)
		for (unsigned byte = 0;
		     byte < cpu.vectors[vector].bytes.size(); ++byte)
			cpu.vectors[vector].bytes[byte] =
				static_cast<std::byte>(
					(vector * 29U + byte * 17U + salt) & 0xffU);
	return cpu;
}

inline void fill_page(Page &page, unsigned salt = 0)
{
	for (std::size_t index = 0; index < page.size(); ++index)
		page[index] = static_cast<std::byte>(
			(index * 37U + salt * 13U + 11U) & 0xffU);
}

inline void store_u64(Page &page, std::size_t offset, std::uint64_t value)
{
	require(offset <= page.size() - sizeof(value));
	for (unsigned byte = 0; byte < sizeof(value); ++byte)
		page[offset + byte] =
			static_cast<std::byte>(value >> (byte * 8));
}

[[nodiscard]] inline test::MemoryRegion memory_region(
	const Page &initial, const Page &expected, bool readable = true,
	bool writable = true, std::uint64_t address = data_base)
{
	return {
		.address = address,
		.initial = initial,
		.expected = expected,
		.readable = readable,
		.writable = writable,
	};
}

[[nodiscard]] constexpr std::uint64_t read_register(
	const arm64::CpuState &cpu, unsigned index, bool stack_pointer) noexcept
{
	return index == 31U ? (stack_pointer ? cpu.sp : 0) :
		cpu.registers[index];
}

inline void write_register(
	arm64::CpuState &cpu, unsigned index, std::uint64_t value,
	bool stack_pointer) noexcept
{
	if (index == 31U) {
		if (stack_pointer)
			cpu.sp = value;
	} else {
		cpu.registers[index] = value;
	}
}

inline void validate_inserted_code(
	const arm64::CpuState &initial,
	const arm64::CpuState &expected_template,
	const arm64::CpuState &actual,
	std::string_view case_name, std::string_view engine,
	std::uint32_t instruction)
{
	unsigned destination = instruction & 31U;
	bool data = false;
	bool authenticate = false;
	if ((instruction & 0xffff'c000U) == 0xdac1'0000U) {
		data = (((instruction >> 10) & 2U) != 0U);
		authenticate = ((instruction >> 10) & 4U) != 0U;
	} else {
		const unsigned immediate = (instruction >> 5) & 0x7fU;
		destination = immediate < 16U ? 17U : 30U;
		authenticate = immediate == 12U || immediate == 14U ||
			(immediate >= 28U && immediate <= 31U);
	}
	auto expected = initial;
	expected.pc = expected_template.pc;
	if (destination < 31U) {
		const auto before = initial.registers[destination];
		const auto after = actual.registers[destination];
		const auto retained = authenticate ?
			strip(before, data) : signing_original(before, data);
		if (strip(after, data) != retained)
			std::fprintf(stderr,
				"%.*s: %.*s retained pointer mismatch before=%#llx after=%#llx data=%u\n",
				static_cast<int>(case_name.size()), case_name.data(),
				static_cast<int>(engine.size()), engine.data(),
				static_cast<unsigned long long>(before),
				static_cast<unsigned long long>(after),
				static_cast<unsigned>(data));
		require(strip(after, data) == retained);
		expected.registers[destination] = after;
	}
	test::require_architecture_equal(
		expected, actual, case_name, engine, instruction);
}

inline void validate_generic_code(
	const arm64::CpuState &initial,
	const arm64::CpuState &expected_template,
	const arm64::CpuState &actual,
	std::string_view case_name, std::string_view engine,
	std::uint32_t instruction)
{
	const unsigned destination = instruction & 31U;
	auto expected = initial;
	expected.pc = expected_template.pc;
	if (destination < 31U) {
		require((actual.registers[destination] & 0xffff'ffffULL) == 0);
		expected.registers[destination] = actual.registers[destination];
	}
	test::require_architecture_equal(
		expected, actual, case_name, engine, instruction);
}

inline void validate_branch_failure(
	const arm64::CpuState &initial,
	const arm64::CpuState &,
	const arm64::CpuState &actual,
	std::string_view case_name, std::string_view engine,
	std::uint32_t instruction)
{
	auto expected = initial;
	require(strip(actual.pc, false) == 0);
	expected.pc = actual.pc;
	if ((instruction & 0x0020'0000U) != 0)
		expected.registers[30] = test::engine_test_code_base + 4;
	test::require_architecture_equal(
		expected, actual, case_name, engine, instruction);
}

inline void require_counts(
	const test::EngineHarness &harness, std::size_t cases)
{
	const auto &counts = harness.counts();
	require(counts.interpreter == cases);
	require(counts.gadgets == cases);
	require(counts.jit + counts.jit_skipped == cases);
	require(counts.jit_unavailable == 0);
}

inline void check_unavailable_pauth_lr(
	test::EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = initial,
		.event = test::ExpectedEvent::illegal_instruction,
		.native = native_pauth_lr,
	});
}

} // namespace arm64_pauth_test
