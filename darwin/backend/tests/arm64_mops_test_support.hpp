// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_load_store_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace mops_test {

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;
namespace test = arm64_load_store_test;

constexpr std::uint64_t pc = 0x7300'0000ULL;

enum class Stage : unsigned {
	prologue,
	main,
	epilogue,
};

[[nodiscard]] constexpr std::uint32_t copy_instruction(bool general,
	Stage stage, unsigned options, unsigned destination, unsigned source,
	unsigned count) noexcept
{
	return (general ? 0x1d00'0400U : 0x1900'0400U) |
		(static_cast<unsigned>(stage) << 22) |
		((source & 31U) << 16) | ((options & 15U) << 12) |
		((count & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t set_instruction(Stage stage,
	unsigned options, unsigned destination, unsigned count,
	unsigned source) noexcept
{
	return 0x19c0'0400U | (static_cast<unsigned>(stage) << 14) |
		((source & 31U) << 16) | ((options & 3U) << 12) |
		((count & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t set_tag_instruction(Stage stage,
	unsigned options, unsigned destination, unsigned count,
	unsigned source) noexcept
{
	return 0x1dc0'0400U | (static_cast<unsigned>(stage) << 14) |
		((source & 31U) << 16) | ((options & 3U) << 12) |
		((count & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr bool valid_copy_registers(unsigned destination,
	unsigned source, unsigned count) noexcept
{
	return destination != 31 && source != 31 && count != 31 &&
		destination != source && destination != count && source != count;
}

[[nodiscard]] constexpr bool valid_set_registers(unsigned destination,
	unsigned count, unsigned source) noexcept
{
	return destination != 31 && count != 31 && destination != count &&
		source != destination && source != count;
}

class Memory final : public arm64::GuestMemory {
public:
	static constexpr std::uint64_t base = 0x0010'0000ULL;
	static constexpr std::size_t length = 128 * 1024;

	Memory() : bytes(length) {}

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> output) override
	{
		++reads;
		last_address = address;
		last_size = output.size();
		if (fault_read != no_fault && address <= fault_read &&
		    fault_read < address + output.size())
			return false;
		if (!contains(address, output.size()))
			return false;
		std::copy_n(at(address), output.size(), output.begin());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> input) override
	{
		++writes;
		last_address = address;
		last_size = input.size();
		if (fault_write != no_fault && address <= fault_write &&
		    fault_write < address + input.size())
			return false;
		if (!contains(address, input.size()))
			return false;
		std::copy(input.begin(), input.end(), at(address));
		return true;
	}

	[[nodiscard]] bool contains(std::uint64_t address,
		std::size_t size = 1) const noexcept
	{
		return address >= base && address - base <= bytes.size() &&
			size <= bytes.size() - static_cast<std::size_t>(address - base);
	}

	[[nodiscard]] std::byte *at(std::uint64_t address)
	{
		test::require(contains(address, 0));
		return bytes.data() + (address - base);
	}

	[[nodiscard]] const std::byte *at(std::uint64_t address) const
	{
		test::require(contains(address, 0));
		return bytes.data() + (address - base);
	}

	void reset_counts() noexcept
	{
		reads = 0;
		writes = 0;
		last_address = 0;
		last_size = 0;
	}

	static constexpr std::uint64_t no_fault =
		std::numeric_limits<std::uint64_t>::max();
	std::vector<std::byte> bytes;
	std::uint64_t fault_read{no_fault};
	std::uint64_t fault_write{no_fault};
	std::uint64_t last_address{};
	std::size_t last_size{};
	std::size_t reads{};
	std::size_t writes{};
};

inline void fill_pattern(std::span<std::byte> bytes, unsigned salt = 0)
{
	for (unsigned index = 0; index < bytes.size(); ++index)
		bytes[index] = static_cast<std::byte>(
			(index * 53U + salt * 29U + 0x37U) & 0xffU);
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

inline void require_state(const arm64::CpuState &actual,
	const arm64::CpuState &expected, std::uint64_t expected_pc)
{
	const bool scalar_equal = actual.registers == expected.registers &&
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
		actual.fp_dirty == expected.fp_dirty;
	if (!scalar_equal) {
		std::fprintf(stderr,
			"state mismatch pc=%#llx expected=%#llx nzcv=%#x expected=%#x\n",
			static_cast<unsigned long long>(actual.pc),
			static_cast<unsigned long long>(expected_pc), actual.nzcv,
				expected.nzcv);
		std::fprintf(stderr,
			"sp=%#llx/%#llx fp=%u,%u/%u,%u exclusive=%#llx,%#llx/%#llx,%#llx\n",
			static_cast<unsigned long long>(actual.sp),
			static_cast<unsigned long long>(expected.sp), actual.fp_loaded,
			actual.fp_dirty, expected.fp_loaded, expected.fp_dirty,
			static_cast<unsigned long long>(actual.exclusive_address),
			static_cast<unsigned long long>(actual.exclusive_key),
			static_cast<unsigned long long>(expected.exclusive_address),
			static_cast<unsigned long long>(expected.exclusive_key));
		for (unsigned index = 0; index < actual.registers.size(); ++index)
			if (actual.registers[index] != expected.registers[index])
				std::fprintf(stderr,
					"x%u=%#llx expected=%#llx\n", index,
					static_cast<unsigned long long>(actual.registers[index]),
					static_cast<unsigned long long>(expected.registers[index]));
	}
	test::require(scalar_equal);
	for (unsigned index = 0; index < actual.vectors.size(); ++index)
		test::require(actual.vectors[index].bytes ==
			expected.vectors[index].bytes);
}

inline void require_continued(const arm64::RunResult &result,
	std::uint64_t address, std::uint32_t instruction)
{
	test::require(result.reason == arm64::StopReason::continued &&
		result.address == address && result.instruction == instruction &&
		result.instructions == 1 && !result.write);
}

} // namespace mops_test
