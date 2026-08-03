// SPDX-License-Identifier: GPL-2.0-or-later
#include "../dispatcher.hpp"

#include "../detail/instruction_helpers.hpp"

#include <cstdint>

namespace uml::cpu::arm64 {

namespace {

[[nodiscard]] std::uint32_t crc32_software(std::uint32_t accumulator,
					   std::uint64_t value,
					   unsigned byte_count,
					   std::uint32_t polynomial)
{
	for (unsigned byte = 0; byte < byte_count; ++byte) {
		accumulator ^= static_cast<std::uint8_t>(value >> (byte * 8));
		for (unsigned bit = 0; bit < 8; ++bit)
			accumulator = (accumulator >> 1) ^
				((accumulator & 1U) != 0 ? polynomial : 0);
	}
	return accumulator;
}

} // namespace

RunResult Dispatcher::crc32(CpuState &cpu, GuestMemory &,
			    std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned operation = (instruction >> 10) & 0x7U;
	const unsigned size = operation & 0x3U;
	const bool is_64_bit = (instruction & 0x8000'0000U) != 0;
	if (is_64_bit != (size == 3))
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};

	const auto accumulator = static_cast<std::uint32_t>(
		cpu.read_register((instruction >> 5) & 0x1fU, false));
	const auto data = cpu.read_register((instruction >> 16) & 0x1fU, false);
	const unsigned byte_count = 1U << size;
	const std::uint32_t polynomial = operation < 4U ?
		0xedb8'8320U : 0x82f6'3b78U;
	const auto result = crc32_software(
		accumulator, data, byte_count, polynomial);
	cpu.write_register(instruction & 0x1fU, result, false);
	cpu.pc += 4;
	return detail::continue_after(address, instruction);
}

} // namespace uml::cpu::arm64
