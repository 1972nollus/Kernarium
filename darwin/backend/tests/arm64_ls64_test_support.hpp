// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_load_store_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace ls64_test {

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;
namespace test = arm64_load_store_test;

constexpr std::uint64_t pc = 0x6c64'0000ULL;
constexpr std::uint64_t interpreter_address = 0x1234'5600ULL;

enum class Operation : unsigned {
	load,
	store,
	store_status,
	store_status_accdata,
};

[[nodiscard]] constexpr std::uint32_t instruction(Operation operation,
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

[[nodiscard]] constexpr bool valid_first(unsigned first) noexcept
{
	return first < 24 && (first & 1U) == 0;
}

class ProbeMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> output) override
	{
		last_address = address;
		last_size = output.size();
		++reads;
		if (fault_read)
			return false;
		test::require(output.size() == payload.size());
		std::copy(payload.begin(), payload.end(), output.begin());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> input) override
	{
		last_address = address;
		last_size = input.size();
		++writes;
		if (fault_write)
			return false;
		test::require(input.size() == written.size());
		std::copy(input.begin(), input.end(), written.begin());
		return true;
	}

	void reset_counts() noexcept
	{
		reads = 0;
		writes = 0;
		last_address = 0;
		last_size = 0;
	}

	std::array<std::byte, 64> payload{};
	std::array<std::byte, 64> written{};
	std::uint64_t last_address{};
	std::size_t last_size{};
	std::size_t reads{};
	std::size_t writes{};
	bool fault_read{};
	bool fault_write{};
};

inline void fill_payload(std::span<std::byte, 64> bytes, unsigned salt = 0)
{
	for (unsigned index = 0; index < bytes.size(); ++index)
		bytes[index] = static_cast<std::byte>(
			(index * 37U + salt * 19U + 0x5bU) & 0xffU);
}

[[nodiscard]] inline std::uint64_t little_endian_word(
	std::span<const std::byte, 8> bytes) noexcept
{
	std::uint64_t value{};
	for (unsigned index = 0; index < bytes.size(); ++index)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(bytes[index])) << (index * 8);
	return value;
}

inline void encode_word(std::uint64_t value, std::span<std::byte, 8> bytes)
{
	for (unsigned index = 0; index < bytes.size(); ++index)
		bytes[index] = static_cast<std::byte>(value >> (index * 8));
}

[[nodiscard]] inline arm64::CpuState state()
{
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	for (unsigned vector = 0; vector < cpu.vectors.size(); ++vector)
		for (unsigned byte = 0; byte < cpu.vectors[vector].bytes.size(); ++byte)
			cpu.vectors[vector].bytes[byte] = static_cast<std::byte>(
				vector * 17U + byte * 11U);
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.set_mte_configuration(true, 0x8421U);
	cpu.set_mte_generator(true, 0xace1U, 7, 0xfeed'face'0123'4567ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0xa1b2'c3d4U;
	cpu.exclusive_address = 0x7654'3000ULL;
	cpu.exclusive_key = 0x1234U;
	cpu.exclusive_value = 0xa5a5'a5a5'a5a5'a5a5ULL;
	cpu.exclusive_value2 = 0x5a5a'5a5a'5a5a'5a5aULL;
	cpu.exclusive_version = 19;
	cpu.exclusive_size = 16;
	return cpu;
}

inline void set_base(arm64::CpuState &cpu, unsigned base,
	std::uint64_t address) noexcept
{
	if (base == 31)
		cpu.sp = address;
	else
		cpu.registers[base] = address;
}

inline void require_state(const arm64::CpuState &actual,
	const arm64::CpuState &expected, std::uint64_t expected_pc)
{
	test::require(actual.registers == expected.registers &&
		actual.sp == expected.sp && actual.pc == expected_pc &&
		actual.nzcv == expected.nzcv && actual.fpcr == expected.fpcr &&
		actual.fpsr == expected.fpsr && actual.tls == expected.tls &&
		actual.mte_enabled == expected.mte_enabled &&
		actual.mte_exclude == expected.mte_exclude &&
		actual.mte_tag_choices == expected.mte_tag_choices &&
		actual.mte_random_tags == expected.mte_random_tags &&
		actual.mte_seed == expected.mte_seed &&
		actual.mte_tag == expected.mte_tag &&
		actual.mte_random_state == expected.mte_random_state &&
		actual.cpa_enabled == expected.cpa_enabled &&
		actual.cpa_multiply_enabled == expected.cpa_multiply_enabled &&
		actual.accdata == expected.accdata &&
		actual.exclusive_address == expected.exclusive_address &&
		actual.exclusive_key == expected.exclusive_key &&
		actual.exclusive_value == expected.exclusive_value &&
		actual.exclusive_value2 == expected.exclusive_value2 &&
		actual.exclusive_version == expected.exclusive_version &&
		actual.exclusive_size == expected.exclusive_size &&
		actual.deferred_vectors == expected.deferred_vectors &&
		actual.deferred_fpcr == expected.deferred_fpcr &&
		actual.deferred_fpsr == expected.deferred_fpsr &&
		actual.fp_loaded == expected.fp_loaded &&
		actual.fp_dirty == expected.fp_dirty);
	for (unsigned index = 0; index < actual.vectors.size(); ++index)
		test::require(actual.vectors[index].bytes ==
			expected.vectors[index].bytes);
}

inline void apply_expected_load(arm64::CpuState &expected, unsigned first,
	std::span<const std::byte, 64> payload)
{
	for (unsigned index = 0; index < 8; ++index)
		expected.registers[first + index] = little_endian_word(
			std::span<const std::byte, 8>{payload.data() + index * 8, 8});
}

[[nodiscard]] inline std::array<std::byte, 64> expected_store(
	const arm64::CpuState &initial, unsigned first, bool use_accdata)
{
	std::array<std::byte, 64> bytes{};
	for (unsigned index = 0; index < 8; ++index) {
		auto value = initial.registers[first + index];
		if (use_accdata && index == 0)
			value = (value & 0xffff'ffff'0000'0000ULL) | initial.accdata;
		encode_word(value,
			std::span<std::byte, 8>{bytes.data() + index * 8, 8});
	}
	return bytes;
}

} // namespace ls64_test
