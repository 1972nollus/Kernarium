// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstdint>

namespace base_bit_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

inline constexpr std::uint64_t seed_constant = 0x5a6b'7c8d'9eaf'b0c1ULL;

[[nodiscard]] constexpr bool allocated(bool wide, unsigned operation) noexcept
{
	if (operation <= 2U)
		return true;
	if (operation == 3U)
		return wide;
	return operation <= 8U;
}

[[nodiscard]] constexpr std::uint32_t instruction(bool wide,
	unsigned operation, unsigned source, unsigned destination) noexcept
{
	return 0x5ac0'0000U | (wide ? 0x8000'0000U : 0U) |
		((operation & 31U) << 10) | ((source & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirement(unsigned operation) noexcept
{
	if (operation >= 6U)
		return engine_tests::NativeRequirements::features(
			::uml::cpu::ArmFeature::cssc);
	return engine_tests::NativeRequirements::baseline();
}

[[nodiscard]] inline arm64::CpuState state(std::uint64_t seed = seed_constant)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			seed ^ (0x0102'0304'0506'0708ULL * (index + 1U));
	cpu.sp = seed ^ 0x7654'3210'fedc'ba98ULL;
	cpu.nzcv = 0xb000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.tls = seed ^ 0x1234'5678'9abc'def0ULL;
	for (unsigned index = 0; index < cpu.vectors.size(); ++index)
		for (unsigned byte = 0; byte < cpu.vectors[index].bytes.size();
		     ++byte)
			cpu.vectors[index].bytes[byte] =
				static_cast<std::byte>(seed + index * 17U + byte);
	return cpu;
}

[[nodiscard]] constexpr std::uint64_t width_mask(bool wide) noexcept
{
	return wide ? ~std::uint64_t{0} : std::uint64_t{0xffff'ffffU};
}

[[nodiscard]] constexpr std::uint64_t reverse_groups(std::uint64_t value,
	unsigned width, unsigned group_bytes) noexcept
{
	std::uint64_t result = 0;
	for (unsigned group = 0; group < width / 8U; group += group_bytes)
		for (unsigned byte = 0; byte < group_bytes; ++byte) {
			const auto source = group + byte;
			const auto destination =
				group + group_bytes - 1U - byte;
			result |= ((value >> (source * 8U)) & 0xffU) <<
				(destination * 8U);
		}
	return result;
}

[[nodiscard]] constexpr std::uint64_t result(bool wide, unsigned operation,
	std::uint64_t input) noexcept
{
	const unsigned width = wide ? 64U : 32U;
	const auto mask = width_mask(wide);
	const auto value = input & mask;
	switch (operation) {
	case 0: {
		std::uint64_t reversed = 0;
		for (unsigned bit = 0; bit < width; ++bit)
			reversed |= ((value >> bit) & 1U) <<
				(width - 1U - bit);
		return reversed;
	}
	case 1:
	case 2:
	case 3:
		return reverse_groups(value, width, 1U << operation);
	case 4: {
		unsigned count = 0;
		while (count < width &&
		       ((value >> (width - 1U - count)) & 1U) == 0U)
			++count;
		return count;
	}
	case 5: {
		const auto sign = (value >> (width - 1U)) & 1U;
		unsigned count = 0;
		while (count + 1U < width &&
		       ((value >> (width - 2U - count)) & 1U) == sign)
			++count;
		return count;
	}
	case 6: {
		unsigned count = 0;
		while (count < width && ((value >> count) & 1U) == 0U)
			++count;
		return count;
	}
	case 7: {
		unsigned count = 0;
		for (unsigned bit = 0; bit < width; ++bit)
			count += static_cast<unsigned>((value >> bit) & 1U);
		return count;
	}
	case 8:
		return (value >> (width - 1U)) == 0 ?
			value : (~value + 1U) & mask;
	default:
		return 0;
	}
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, bool wide, unsigned operation,
	unsigned source, unsigned destination)
{
	auto expected = initial;
	const auto input = source == 31U ? std::uint64_t{0} :
		initial.registers[source];
	if (destination != 31U)
		expected.registers[destination] =
			result(wide, operation, input) & width_mask(wide);
	return expected;
}

} // namespace base_bit_test
