// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>
#include <string_view>

namespace arm64_base_ls64_test {

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;
namespace test = uml::backend::tests;

inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint64_t data_base = test::engine_test_data_base;
inline constexpr std::size_t transfer_offset = page_size / 2;

using Page = std::array<std::byte, page_size>;

enum class Operation {
	load,
	store,
	store_status,
	store_status_accdata,
};

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

[[nodiscard]] constexpr std::uint32_t encode(Operation operation,
	unsigned first, unsigned base, unsigned status = 31) noexcept
{
	switch (operation) {
	case Operation::load:
		return 0xf83f'd000U | ((base & 31U) << 5) | (first & 31U);
	case Operation::store:
		return 0xf83f'9000U | ((base & 31U) << 5) | (first & 31U);
	case Operation::store_status:
		return 0xf820'b000U | ((status & 31U) << 16) |
			((base & 31U) << 5) | (first & 31U);
	case Operation::store_status_accdata:
		return 0xf820'a000U | ((status & 31U) << 16) |
			((base & 31U) << 5) | (first & 31U);
	}
	return 0;
}

[[nodiscard]] constexpr bool is_load(Operation operation) noexcept
{
	return operation == Operation::load;
}

[[nodiscard]] constexpr bool has_status(Operation operation) noexcept
{
	return operation == Operation::store_status ||
		operation == Operation::store_status_accdata;
}

[[nodiscard]] constexpr bool uses_accdata(Operation operation) noexcept
{
	return operation == Operation::store_status_accdata;
}

[[nodiscard]] constexpr bool valid_first(unsigned first) noexcept
{
	return first < 24U && (first & 1U) == 0U;
}

[[nodiscard]] inline test::NativeRequirements
native_requirements(Operation operation) noexcept
{
	switch (operation) {
	case Operation::load:
	case Operation::store:
		return test::NativeRequirements::features(
			uml::backend::ArmFeature::ls64);
	case Operation::store_status:
		return test::NativeRequirements::features(
			uml::backend::ArmFeature::ls64_v);
	case Operation::store_status_accdata:
		return test::NativeRequirements::features(
			uml::backend::ArmFeature::ls64_accdata);
	}
	return test::NativeRequirements::baseline();
}

[[nodiscard]] inline arm64::CpuState initial_state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			0x1020'3040'5060'7000ULL +
			static_cast<std::uint64_t>(index) * 0x0101'1021'3243'5465ULL;
	cpu.sp = data_base + transfer_offset;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0010U;
	cpu.fpmr = 0x1020'3040'5060'7080ULL;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
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
	for (unsigned vector = 0; vector < cpu.vectors.size(); ++vector)
		for (unsigned byte = 0;
		     byte < cpu.vectors[vector].bytes.size(); ++byte)
			cpu.vectors[vector].bytes[byte] = static_cast<std::byte>(
				(vector * 29U + byte * 17U + 3U) & 0xffU);
	return cpu;
}

inline void set_base(arm64::CpuState &cpu, unsigned base,
	std::uint64_t address) noexcept
{
	if (base == 31U)
		cpu.sp = address;
	else
		cpu.registers[base] = address;
}

inline void fill_pattern(Page &page, unsigned salt = 0)
{
	for (std::size_t index = 0; index < page.size(); ++index)
		page[index] = static_cast<std::byte>(
			(index * 37U + salt * 19U + 0x5bU) & 0xffU);
}

[[nodiscard]] inline std::uint64_t load_u64(
	const Page &page, std::size_t offset)
{
	require(offset <= page.size() - sizeof(std::uint64_t));
	std::uint64_t value = 0;
	for (unsigned byte = 0; byte < sizeof(value); ++byte)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned char>(page[offset + byte]))
			<< (byte * 8);
	return value;
}

inline void store_u64(Page &page, std::size_t offset, std::uint64_t value)
{
	require(offset <= page.size() - sizeof(value));
	for (unsigned byte = 0; byte < sizeof(value); ++byte)
		page[offset + byte] =
			static_cast<std::byte>(value >> (byte * 8));
}

inline void apply_load_oracle(
	arm64::CpuState &expected, unsigned first, const Page &memory)
{
	for (unsigned index = 0; index < 8; ++index)
		expected.registers[first + index] = load_u64(
			memory, transfer_offset + index * sizeof(std::uint64_t));
}

inline void apply_store_oracle(Page &expected_memory,
	arm64::CpuState &expected_cpu, const arm64::CpuState &initial,
	Operation operation, unsigned first, unsigned status)
{
	for (unsigned index = 0; index < 8; ++index) {
		auto value = initial.registers[first + index];
		if (uses_accdata(operation) && index == 0)
			value = (value & 0xffff'ffff'0000'0000ULL) |
				initial.accdata;
		store_u64(expected_memory,
			transfer_offset + index * sizeof(std::uint64_t), value);
	}
	/* Normal mapped emulator memory is an LS64-capable location and chooses
	 * zero for its implementation-defined successful status value.
	 */
	if (has_status(operation) && status != 31U)
		expected_cpu.registers[status] = 0;
}

[[nodiscard]] inline test::MemoryRegion memory_region(
	const Page &initial, const Page &expected, bool readable = true,
	bool writable = true,
	std::span<const test::AllocationTag> initial_tags = {},
	std::span<const test::AllocationTag> expected_tags = {})
{
	return {
		.address = data_base,
		.initial = initial,
		.expected = expected,
		.initial_tags = initial_tags,
		.expected_tags = expected_tags,
		.readable = readable,
		.writable = writable,
	};
}

inline void require_counts(const test::EngineHarness &harness,
	std::size_t cases)
{
	const auto &counts = harness.counts();
	require(counts.interpreter == cases);
	require(counts.gadgets == cases);
	require(counts.jit + counts.jit_skipped == cases);
}

} // namespace arm64_base_ls64_test
