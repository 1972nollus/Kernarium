// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/instruction_helpers.hpp"

#include <bit>
#include <cstdint>
#include <limits>

namespace uml::cpu::arm64 {

using detail::continue_after;

namespace {

[[nodiscard]] std::uint64_t pointer_check(const CpuState &cpu,
	std::uint64_t result, std::uint64_t base,
	bool multiplication_overflow) noexcept
{
	const bool previous_detection = ((base >> 55) & 1U) !=
		((base >> 54) & 1U);
	const bool addition_detected = (result >> 56) != (base >> 56) ||
		previous_detection;
	const bool detected = (cpu.cpa_enabled && addition_detected) ||
		(cpu.cpa_multiply_enabled && multiplication_overflow);
	if (!detected)
		return result;
	constexpr auto low_mask = (std::uint64_t{1} << 54) - 1;
	const auto inverted_base_55 = ((~base >> 55) & 1U) << 54;
	return (result & low_mask) | (base & 0xff80'0000'0000'0000ULL) |
		inverted_base_55;
}

} // namespace

RunResult Dispatcher::cpa_pointer(CpuState &cpu, GuestMemory &,
				  std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool subtract = (instruction & 0x4000'0000U) != 0;
	const unsigned destination = instruction & 31U;
	const unsigned base_register = (instruction >> 5) & 31U;
	const unsigned shift = (instruction >> 10) & 7U;
	const unsigned offset_register = (instruction >> 16) & 31U;
	const auto base = cpu.read_register(base_register, true);
	const auto offset = cpu.read_register(offset_register, false) << shift;
	const auto arithmetic = subtract ? base - offset : base + offset;
	const auto result = pointer_check(cpu, arithmetic, base, false);

	cpu.write_register(destination, result, true);
	cpu.pc += sizeof(instruction);
	return continue_after(address, instruction);
}

RunResult Dispatcher::cpa_multiply(CpuState &cpu, GuestMemory &,
				   std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool subtract = (instruction & 0x0000'8000U) != 0;
	const unsigned destination = instruction & 31U;
	const unsigned left_register = (instruction >> 5) & 31U;
	const unsigned addend_register = (instruction >> 10) & 31U;
	const unsigned right_register = (instruction >> 16) & 31U;
	const auto left = cpu.read_register(left_register, false);
	const auto right = cpu.read_register(right_register, false);
	const auto addend = cpu.read_register(addend_register, false);

	/* Use a wide signed product in the diagnostic interpreter so overflow is
	 * detected directly, without relying on host flags or signed overflow.
	 */
	const auto product = static_cast<__int128>(
		std::bit_cast<std::int64_t>(left)) * static_cast<__int128>(
		std::bit_cast<std::int64_t>(right));
	const bool overflow = product < std::numeric_limits<std::int64_t>::min() ||
		product > std::numeric_limits<std::int64_t>::max();
	const auto low_product = static_cast<std::uint64_t>(product);
	const auto arithmetic = subtract ? addend - low_product :
		addend + low_product;
	const auto result = pointer_check(cpu, arithmetic, addend, overflow);

	cpu.write_register(destination, result, false);
	cpu.pc += sizeof(instruction);
	return continue_after(address, instruction);
}

} // namespace uml::cpu::arm64
