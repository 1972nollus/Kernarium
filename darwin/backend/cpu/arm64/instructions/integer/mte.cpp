// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/instruction_helpers.hpp"

#include <bit>
#include <cstdint>

namespace uml::backend::arm64 {

using detail::continue_after;
using detail::set_add_sub_flags;
using detail::sign_extend;

namespace {

[[nodiscard]] unsigned population_count(std::uint16_t value) noexcept
{
	return std::popcount(value);
}

[[nodiscard]] std::uint8_t select_allowed_tag(
	std::uint16_t allowed, unsigned selection) noexcept
{
	for (unsigned tag = 0; tag < 16; ++tag) {
		if ((allowed & (1U << tag)) == 0)
			continue;
		if (selection-- == 0)
			return static_cast<std::uint8_t>(tag);
	}
	return 0;
}

[[nodiscard]] std::uint8_t choose_irg_tag(
	CpuState &cpu, std::uint16_t exclude) noexcept
{
	if (!cpu.mte_enabled || exclude == 0xffffU)
		return 0;
	const auto allowed = static_cast<std::uint16_t>(~exclude);
	const unsigned count = population_count(allowed);
	unsigned selection;
	if (cpu.mte_random_tags) {
		auto random = cpu.mte_random_state;
		random ^= random << 13;
		random ^= random >> 7;
		random ^= random << 17;
		cpu.mte_random_state = random;
		selection = static_cast<unsigned>(random % count);
	} else {
		unsigned generator_tag = static_cast<unsigned>(cpu.mte_tag) + 1U;
		if (generator_tag >= count)
			generator_tag = 0;
		cpu.mte_tag = static_cast<std::uint8_t>(generator_tag);
		selection = static_cast<unsigned>(
			(generator_tag + count * static_cast<unsigned>(cpu.mte_seed)) >> 16) %
			count;
		const unsigned feedback = ((cpu.mte_seed >> 5) ^
			(cpu.mte_seed >> 3) ^ (cpu.mte_seed >> 2) ^ cpu.mte_seed) & 1U;
		cpu.mte_seed = static_cast<std::uint16_t>(
			(cpu.mte_seed >> 1) | (feedback << 15));
	}
	return select_allowed_tag(allowed, selection);
}

} // namespace

RunResult Dispatcher::mte_add_sub_tag(CpuState &cpu, GuestMemory &,
				      std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool subtract = (instruction & 0x4000'0000U) != 0;
	const unsigned destination = instruction & 31U;
	const unsigned source_register = (instruction >> 5) & 31U;
	const unsigned tag_offset = (instruction >> 10) & 15U;
	const auto offset = static_cast<std::uint64_t>(
		(instruction >> 16) & 63U) << 4;
	const auto source = cpu.read_register(source_register, true);
	const unsigned start_tag = static_cast<unsigned>(source >> 56) & 15U;
	const auto selected_tag = cpu.choose_mte_tag(start_tag, tag_offset);
	const auto arithmetic = subtract ? source - offset : source + offset;
	const auto result = (arithmetic & ~0x0f00'0000'0000'0000ULL) |
		(static_cast<std::uint64_t>(selected_tag) << 56);

	cpu.write_register(destination, result, true);
	cpu.pc += sizeof(instruction);
	return continue_after(address, instruction);
}

RunResult Dispatcher::mte_pointer(CpuState &cpu, GuestMemory &,
				  std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto opcode = instruction & 0xffe0'fc00U;
	const unsigned destination = instruction & 0x1fU;
	const unsigned first_register = (instruction >> 5) & 0x1fU;
	const unsigned second_register = (instruction >> 16) & 0x1fU;

	/* Exact decoder entries are intentional. They keep the reference path
	 * from treating neighboring reserved encodings as ordinary arithmetic and
	 * guarantee that all operands are read before any overlapping destination
	 * is committed.
	 */
	if (opcode == 0x9ac0'1000U) {
		const auto tagged_address = cpu.read_register(first_register, true);
		const auto register_exclude = cpu.read_register(second_register, false);
		const auto exclude = static_cast<std::uint16_t>(
			cpu.mte_exclude | register_exclude);
		const auto selected_tag = choose_irg_tag(cpu, exclude);
		const auto result = (tagged_address & ~0x0f00'0000'0000'0000ULL) |
			(static_cast<std::uint64_t>(selected_tag) << 56);
		cpu.write_register(destination, result, true);
	} else if (opcode == 0x9ac0'1400U) {
		const auto tagged_address = cpu.read_register(first_register, true);
		const auto mask = cpu.read_register(second_register, false);
		const unsigned tag = static_cast<unsigned>(tagged_address >> 56) & 15U;
		cpu.write_register(destination, mask | (std::uint64_t{1} << tag), false);
	} else {
		const auto first = static_cast<std::uint64_t>(sign_extend(
			cpu.read_register(first_register, true) & 0x00ff'ffff'ffff'ffffULL,
			56));
		const auto second = static_cast<std::uint64_t>(sign_extend(
			cpu.read_register(second_register, true) & 0x00ff'ffff'ffff'ffffULL,
			56));
		const auto result = first - second;
		if (opcode == 0xbac0'0000U)
			set_add_sub_flags(cpu, first, second, result, true, true);
		cpu.write_register(destination, result, false);
	}

	cpu.pc += sizeof(instruction);
	return continue_after(address, instruction);
}

} // namespace uml::backend::arm64
