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

namespace arm64_base_gcs_test {

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;
namespace test = uml::backend::tests;

inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint64_t data_base = test::engine_test_data_base;
inline const auto native_gcs =
	test::NativeRequirements::features(uml::backend::ArmFeature::gcs);

using Page = std::array<std::byte, page_size>;

enum class Operation {
	pop_exception_context,
	pop_main,
	pop_exception,
	push_main,
	push_exception,
	switch_stack_1,
	switch_stack_2,
	store,
	store_unprivileged,
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
	std::array<char, 160> storage_{};
	std::size_t length_{};
};

[[nodiscard]] constexpr std::uint32_t encode(
	Operation operation, unsigned transfer, unsigned base = 0) noexcept
{
	switch (operation) {
	case Operation::pop_exception_context:
		return 0xd508'77a0U | (transfer & 31U);
	case Operation::pop_main:
		return 0xd52b'7720U | (transfer & 31U);
	case Operation::pop_exception:
		return 0xd508'77c0U | (transfer & 31U);
	case Operation::push_main:
		return 0xd50b'7700U | (transfer & 31U);
	case Operation::push_exception:
		return 0xd508'7780U | (transfer & 31U);
	case Operation::switch_stack_1:
		return 0xd50b'7740U | (transfer & 31U);
	case Operation::switch_stack_2:
		return 0xd52b'7760U | (transfer & 31U);
	case Operation::store:
		return 0xd91f'0c00U | ((base & 31U) << 5) |
			(transfer & 31U);
	case Operation::store_unprivileged:
		return 0xd91f'1c00U | ((base & 31U) << 5) |
			(transfer & 31U);
	}
	return 0;
}

[[nodiscard]] inline arm64::CpuState initial_state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			0x1020'3040'5060'7000ULL +
			static_cast<std::uint64_t>(index) * 0x1010'101ULL;
	cpu.sp = data_base + page_size / 2;
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
	cpu.gcs_pointer = (data_base + page_size / 2) & ~std::uint64_t{7};
	cpu.gcs_implemented = true;
	cpu.gcs_push_enabled = true;
	cpu.gcs_store_enabled = true;
	for (unsigned vector = 0; vector < cpu.vectors.size(); ++vector)
		for (unsigned byte = 0;
		     byte < cpu.vectors[vector].bytes.size(); ++byte)
			cpu.vectors[vector].bytes[byte] = static_cast<std::byte>(
				(vector * 29U + byte * 17U + 3U) & 0xffU);
	return cpu;
}

[[nodiscard]] constexpr std::uint64_t read_register(
	const arm64::CpuState &cpu, unsigned index, bool stack_pointer) noexcept
{
	return index == 31U ? (stack_pointer ? cpu.sp : 0) :
		cpu.registers[index];
}

inline void write_register(arm64::CpuState &cpu, unsigned index,
	std::uint64_t value, bool stack_pointer) noexcept
{
	if (index == 31U) {
		if (stack_pointer)
			cpu.sp = value;
	} else {
		cpu.registers[index] = value;
	}
}

inline void fill_pattern(Page &page, unsigned salt = 0)
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

inline void require_counts(const test::EngineHarness &harness,
	std::size_t cases)
{
	const auto &counts = harness.counts();
	require(counts.interpreter == cases);
	require(counts.gadgets == cases);
	require(counts.jit + counts.jit_skipped == cases);
}

inline void check_allocated_illegal(test::EngineHarness &harness,
	std::string_view name, std::uint32_t instruction,
	const arm64::CpuState &initial)
{
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = initial,
		.event = test::ExpectedEvent::illegal_instruction,
		.native = native_gcs,
	});
}

} // namespace arm64_base_gcs_test
