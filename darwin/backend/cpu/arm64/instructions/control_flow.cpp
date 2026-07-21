// SPDX-License-Identifier: GPL-2.0-or-later
#include "../dispatcher.hpp"

#include "../detail/instruction_helpers.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace uml::backend::arm64 {

using detail::condition_holds;
using detail::continue_after;
using detail::sign_extend;
using detail::width_mask;

RunResult Dispatcher::exception(CpuState &cpu, GuestMemory &,
				std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	if ((instruction & 0xffe0'001fU) == 0xd400'0001U) {
		cpu.pc += 4;
		return {StopReason::system_call, address, instruction, 1, false};
	}
	return {StopReason::breakpoint, address, instruction, 1, false};
}

RunResult Dispatcher::branch_immediate(CpuState &cpu, GuestMemory &,
				       std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	if ((instruction & 0x8000'0000U) != 0)
		cpu.registers[30] = address + 4;
	const auto displacement = sign_extend(
		static_cast<std::uint64_t>(instruction & 0x03ff'ffffU) << 2, 28);
	cpu.pc = static_cast<std::uint64_t>(
		static_cast<std::int64_t>(address) + displacement);
	return continue_after(address, instruction);
}

RunResult Dispatcher::branch_conditional(CpuState &cpu, GuestMemory &,
					 std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto displacement = sign_extend(
		static_cast<std::uint64_t>((instruction >> 5) & 0x7ffffU) << 2,
		21);
	cpu.pc = condition_holds(cpu.nzcv, instruction & 0xfU) ?
		static_cast<std::uint64_t>(
			static_cast<std::int64_t>(address) + displacement) :
		address + 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::compare_branch(CpuState &cpu, GuestMemory &,
				     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool is_64_bit = (instruction >> 31) != 0;
	const bool nonzero = (instruction & 0x0100'0000U) != 0;
	const auto value = cpu.read_register(instruction & 0x1fU, false) &
		width_mask(is_64_bit);
	const bool take = nonzero ? value != 0 : value == 0;
	const auto displacement = sign_extend(
		static_cast<std::uint64_t>((instruction >> 5) & 0x7ffffU) << 2,
		21);
	cpu.pc = take ? static_cast<std::uint64_t>(
		static_cast<std::int64_t>(address) + displacement) : address + 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::test_branch(CpuState &cpu, GuestMemory &,
				  std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned bit = ((instruction >> 31) << 5) |
		((instruction >> 19) & 0x1fU);
	const bool nonzero = (instruction & 0x0100'0000U) != 0;
	const bool set = ((cpu.read_register(instruction & 0x1fU, false) >> bit) &
		1U) != 0;
	const bool take = nonzero ? set : !set;
	const auto displacement = sign_extend(
		static_cast<std::uint64_t>((instruction >> 5) & 0x3fffU) << 2,
		16);
	cpu.pc = take ? static_cast<std::uint64_t>(
		static_cast<std::int64_t>(address) + displacement) : address + 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::branch_register(CpuState &cpu, GuestMemory &,
				      std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto operation = instruction & 0xffff'fc1fU;
	const auto target = cpu.read_register((instruction >> 5) & 0x1fU, false);
	if (operation == 0xd63f'0000U)
		cpu.registers[30] = address + 4;
	cpu.pc = target;
	return continue_after(address, instruction);
}

RunResult Dispatcher::pc_relative(CpuState &cpu, GuestMemory &,
				  std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto immediate = static_cast<std::uint64_t>(
		((instruction >> 5) & 0x7ffffU) << 2) |
		((instruction >> 29) & 0x3U);
	std::uint64_t value;
	if ((instruction & 0x8000'0000U) != 0) {
		value = static_cast<std::uint64_t>(
			static_cast<std::int64_t>(address & ~0xfffULL) +
			sign_extend(immediate, 21) * 4096);
	} else {
		value = static_cast<std::uint64_t>(
			static_cast<std::int64_t>(address) +
			sign_extend(immediate, 21));
	}
	cpu.write_register(instruction & 0x1fU, value, false);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::system_register(CpuState &cpu, GuestMemory &,
				     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned transfer = instruction & 0x1fU;
	const auto system_register = instruction & 0xffff'ffe0U;
	if (system_register == 0xd53b'd040U)
		cpu.write_register(transfer, cpu.tls, false);
	else if (system_register == 0xd51b'd040U)
		cpu.tls = cpu.read_register(transfer, false);
	else if (system_register == 0xd53b'00e0U)
		/* DZP=1 advertises that DC ZVA is unavailable.  The emulator has no
		 * cache-maintenance side channel, so userspace must use ordinary
		 * stores for architecturally visible zeroing.
		 */
		cpu.write_register(transfer, 1U << 4, false);
	else if (system_register == 0xd53b'0020U)
		/* 64-byte I/D cache lines, PIPT L1, and 64-byte exclusive and
		 * cache-writeback granules.  The static dispatcher models the
		 * corresponding userspace cache-maintenance synchronization points.
		 */
		cpu.write_register(transfer, 0x8444'8004U, false);
	else if (system_register == 0xd53b'4400U)
		cpu.write_register(transfer, cpu.fpcr, false);
	else if (system_register == 0xd51b'4400U)
		cpu.fpcr = static_cast<std::uint32_t>(
			cpu.read_register(transfer, false));
	else if (system_register == 0xd53b'4420U)
		cpu.write_register(transfer, cpu.fpsr, false);
	else if (system_register == 0xd51b'4420U)
		cpu.fpsr = static_cast<std::uint32_t>(
			cpu.read_register(transfer, false));
	else if (system_register == 0xd53b'4200U)
		cpu.write_register(transfer, cpu.nzcv, false);
	else if (system_register == 0xd51b'4200U)
		cpu.nzcv = static_cast<std::uint32_t>(
			cpu.read_register(transfer, false)) & 0xf000'0000U;
	else if (system_register == 0xd53b'e000U)
		/* Express the virtual counter directly in nanoseconds.  Pairing a 1 GHz
		 * frequency with steady_clock keeps CNTVCT monotonic and makes the two
		 * architectural registers self-consistent without exposing host timing
		 * details to the guest.
		 */
		cpu.write_register(transfer, 1'000'000'000U, false);
	else if (system_register == 0xd53b'e040U)
		cpu.write_register(transfer, static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count()),
			false);
	else
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::memory_barrier(CpuState &cpu, GuestMemory &,
				     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	std::atomic_thread_fence(std::memory_order_seq_cst);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::cache_maintenance(CpuState &cpu, GuestMemory &memory,
					std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto operation = instruction & 0xffff'ffe0U;

	/* Guest data writes already update the single coherent physical backing.
	 * DC CVAU therefore needs only an ordering edge.  IC IVAU additionally
	 * invalidates translated instruction blocks so self-modifying code becomes
	 * visible at the same architectural synchronization point as on arm64.
	 */
	std::atomic_thread_fence(std::memory_order_seq_cst);
	if (operation == 0xd50b'7520U)
		memory.invalidate_code(
			cpu.read_register(instruction & 0x1fU, false));
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::nop(CpuState &cpu, GuestMemory &,
			  std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

} // namespace uml::backend::arm64
