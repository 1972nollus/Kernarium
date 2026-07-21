// SPDX-License-Identifier: GPL-2.0-or-later
#include "../dispatcher.hpp"

#include "../detail/instruction_helpers.hpp"

#include <arm_acle.h>

#include <cstdint>

namespace uml::backend::arm64 {

__attribute__((target("crc")))
RunResult Dispatcher::crc32(CpuState &cpu, GuestMemory &,
			    std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned operation = (instruction >> 10) & 0x7U;
	const unsigned size = operation & 0x3U;
	const bool is_64_bit = (instruction & 0x8000'0000U) != 0;
	if (is_64_bit != (size == 3))
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};

	const auto accumulator = static_cast<std::uint32_t>(
		cpu.read_register((instruction >> 5) & 0x1fU, false));
	const auto data = cpu.read_register((instruction >> 16) & 0x1fU, false);
	std::uint32_t result;
	switch (operation) {
	case 0: result = __crc32b(accumulator, static_cast<std::uint8_t>(data)); break;
	case 1: result = __crc32h(accumulator, static_cast<std::uint16_t>(data)); break;
	case 2: result = __crc32w(accumulator, static_cast<std::uint32_t>(data)); break;
	case 3: result = __crc32d(accumulator, data); break;
	case 4: result = __crc32cb(accumulator, static_cast<std::uint8_t>(data)); break;
	case 5: result = __crc32ch(accumulator, static_cast<std::uint16_t>(data)); break;
	case 6: result = __crc32cw(accumulator, static_cast<std::uint32_t>(data)); break;
	case 7: result = __crc32cd(accumulator, data); break;
	default: __builtin_unreachable();
	}
	cpu.write_register(instruction & 0x1fU, result, false);
	cpu.pc += 4;
	return detail::continue_after(address, instruction);
}

} // namespace uml::backend::arm64
