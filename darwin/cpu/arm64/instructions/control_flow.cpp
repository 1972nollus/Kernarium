// SPDX-License-Identifier: GPL-2.0-or-later
#include "../dispatcher.hpp"

#include "../detail/host_counter.hpp"
#include "../detail/instruction_helpers.hpp"
#include "../detail/pointer_auth.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace uml::cpu::arm64 {

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
	if ((instruction & 0xffe0'001fU) == 0xd420'0000U)
		return {StopReason::breakpoint, address, instruction, 1, false};
	/* The static guest executes in the architectural EL0, non-Debug context.
	 * HVC, SMC, HLT, DCPS<n>, DRPS, ERET, ERETAA, and ERETAB are therefore all
	 * Undefined without changing PC or general-purpose state.
	 */
	return {StopReason::illegal_instruction, address, instruction, 0, false};
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

RunResult Dispatcher::compare_conditional_branch(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const bool immediate = (instruction & 0x0100'0000U) != 0;
	const unsigned narrow_control = (instruction >> 14) & 3U;
	const bool valid_form = immediate ?
		(instruction & 0x0000'4000U) == 0 :
		(narrow_control == 0 ||
		 (((instruction >> 31) == 0) && narrow_control >= 2));
	const unsigned cc = (instruction >> 21) & 7U;
	if (!valid_form || cc == 4 || cc == 5)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};

	const bool narrow = !immediate && narrow_control >= 2;
	const unsigned width = narrow ?
		((instruction & 0x0000'4000U) != 0 ? 16U : 8U) :
		((instruction & 0x8000'0000U) != 0 ? 64U : 32U);
	const auto mask = width == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << width) - 1;
	const auto left = cpu.read_register(instruction & 31U, false) & mask;
	const auto right = immediate ? (instruction >> 15) & 63U :
		cpu.read_register((instruction >> 16) & 31U, false) & mask;
	const auto signed_left = sign_extend(left, width);
	const auto signed_right = sign_extend(right, width);

	bool take = false;
	switch (cc) {
	case 0: take = signed_left > signed_right; break;
	case 1:
		take = immediate ? signed_left < signed_right :
			signed_left >= signed_right;
		break;
	case 2: take = left > right; break;
	case 3: take = immediate ? left < right : left >= right; break;
	case 6: take = left == right; break;
	case 7: take = left != right; break;
	default: break;
	}

	const auto displacement = sign_extend(
		static_cast<std::uint64_t>((instruction >> 5) & 0x1ffU) << 2,
		11);
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

RunResult Dispatcher::branch_authenticated(CpuState &cpu, GuestMemory &memory,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	if (!cpu.pointer_authentication_enabled)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const bool return_form =
		(instruction & 0xffff'fbffU) == 0xd65f'0bffU;
	const bool key_b = (instruction & 0x400U) != 0;
	const bool link = !return_form && (instruction & 0x0020'0000U) != 0;
	unsigned target_register;
	std::uint64_t modifier;
	bool zero_modifier;
	if (return_form) {
		target_register = 30;
		modifier = cpu.sp;
		zero_modifier = false;
	} else {
		target_register = (instruction >> 5) & 31U;
		const unsigned modifier_register = instruction & 31U;
		zero_modifier = (instruction & 0x0100'0000U) == 0;
		if (zero_modifier && modifier_register != 31U)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		modifier = zero_modifier ? 0 :
			cpu.read_register(modifier_register, true);
	}
	auto target = cpu.read_register(target_register, false);
	target = detail::execute_pointer_auth(
		cpu, target, modifier,
		key_b ? detail::PointerAuthOperation::authenticate_instruction_b :
			detail::PointerAuthOperation::authenticate_instruction_a);
	if (cpu.gcs_implemented && cpu.gcs_push_enabled) {
		if (link) {
			const auto record_address =
				cpu.gcs_pointer - sizeof(std::uint64_t);
			std::array<std::byte, sizeof(std::uint64_t)> bytes{};
			detail::encode_little_endian(address + 4, bytes);
			if (!memory.write(record_address, bytes)) {
				const auto fault = memory.last_fault();
				return {StopReason::memory_fault, fault.address,
					instruction, 0, true};
			}
			cpu.gcs_pointer =
				record_address & ~std::uint64_t{7};
		} else if (return_form) {
			const auto record_address = cpu.gcs_pointer;
			std::array<std::byte, sizeof(std::uint64_t)> bytes{};
			if (!memory.read(record_address, bytes)) {
				const auto fault = memory.last_fault();
				return {StopReason::memory_fault, fault.address,
					instruction, 0, false};
			}
			const auto recorded =
				detail::decode_little_endian(bytes);
			if (recorded != target)
				return {StopReason::gcs_fault, address,
					instruction, 0, false};
			target = recorded;
			cpu.gcs_pointer =
				(record_address + sizeof(recorded)) &
				~std::uint64_t{7};
		}
	}
	if (link)
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
	constexpr std::uint32_t fpcr_mask = 0x07f7'9f07U;
	constexpr std::uint32_t fpsr_mask = 0xf800'009fU;
	constexpr std::uint64_t fpmr_mask = 0x0000'003f'ff7f'c1ffULL;
	const auto address = cpu.pc;
	const unsigned transfer = instruction & 0x1fU;
	const auto system_register = instruction & 0xffff'ffe0U;
	if (system_register == 0xd53b'd040U)
		cpu.write_register(transfer, cpu.tls, false);
	else if (system_register == 0xd51b'd040U)
		cpu.tls = cpu.read_register(transfer, false);
	else if (system_register == 0xd53b'd060U)
		/* Linux does not assign TPIDRRO_EL0 for this UML architecture, but the
		 * architectural EL0 read remains present and reads its reset value. */
		cpu.write_register(transfer, 0, false);
	else if (system_register == 0xd53b'00e0U)
		/* DZP=0 and BS=4 advertise the 64-byte DC ZVA implementation. */
		cpu.write_register(transfer, 4U, false);
	else if (system_register == 0xd53b'0020U)
		/* 64-byte I/D cache lines, PIPT L1, and 64-byte exclusive and
		 * cache-writeback granules.  The static dispatcher models the
		 * corresponding userspace cache-maintenance synchronization points.
		 */
		cpu.write_register(transfer, 0x8444'8004U, false);
	else if (system_register == 0xd53b'4400U)
		cpu.write_register(transfer, cpu.fpcr & fpcr_mask, false);
	else if (system_register == 0xd51b'4400U)
		cpu.fpcr = static_cast<std::uint32_t>(
			cpu.read_register(transfer, false)) & fpcr_mask;
	else if (system_register == 0xd53b'4420U)
		cpu.write_register(transfer, cpu.fpsr & fpsr_mask, false);
	else if (system_register == 0xd51b'4420U)
		cpu.fpsr = static_cast<std::uint32_t>(
			cpu.read_register(transfer, false)) & fpsr_mask;
	else if (system_register == 0xd53b'4440U)
		cpu.write_register(transfer, cpu.fpmr & fpmr_mask, false);
	else if (system_register == 0xd51b'4440U)
		cpu.fpmr = cpu.read_register(transfer, false) & fpmr_mask;
	else if (system_register == 0xd53b'4200U)
		cpu.write_register(transfer, cpu.nzcv, false);
	else if (system_register == 0xd51b'4200U)
		cpu.nzcv = static_cast<std::uint32_t>(
			cpu.read_register(transfer, false)) & 0xf000'0000U;
	else if (system_register == 0xd53b'e000U)
		cpu.write_register(
			transfer, detail::read_host_counter_frequency(), false);
	else if (system_register == 0xd53b'e040U)
		cpu.write_register(
			transfer, detail::read_host_virtual_counter(), false);
	else if (system_register == 0xd53b'42a0U)
		/* DIT reads as Zeros(39):PSTATE.DIT:Zeros(24); the same PSTATE bit
		 * the MSR immediate form writes, so a guest that sets DIT must be
		 * able to read it back through the register form. */
		cpu.write_register(
			transfer, cpu.pstate_dit ? 0x0100'0000ULL : 0ULL, false);
	else if (system_register == 0xd51b'42a0U)
		cpu.pstate_dit =
			(cpu.read_register(transfer, false) & 0x0100'0000ULL) != 0;
	else
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::system_register_pair(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	/* The current EL0 profile exposes no 128-bit system registers. Decode the
	 * whole FEAT_SYSREG128 allocation so odd Rt (reserved) and inaccessible
	 * named/numeric registers all fail before touching architectural state. */
	return {StopReason::illegal_instruction, cpu.pc, instruction, 0, false};
}

RunResult Dispatcher::system_instruction_pair(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	/* FEAT_SYSINSTR128 is not exposed to the static EL0 guest profile. */
	return {StopReason::illegal_instruction, cpu.pc, instruction, 0, false};
}

RunResult Dispatcher::system_instruction(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	/* The two enabled EL0 cache-maintenance operations are decoded to the
	 * dedicated handler before this complete SYS/SYSL allocation. All other
	 * operations either require a higher exception level or an extension not
	 * exposed by this static userspace profile. */
	return {StopReason::illegal_instruction, cpu.pc, instruction, 0, false};
}

RunResult Dispatcher::unavailable_extension(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	return {StopReason::unavailable_feature, cpu.pc, instruction, 0, false};
}

RunResult Dispatcher::memory_barrier(CpuState &cpu, GuestMemory &,
				     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	std::atomic_thread_fence(std::memory_order_seq_cst);
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::clear_exclusive(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	/* CRm is architecturally ignored. Clear every debug representation of the
	 * local monitor so stale state cannot survive malformed test sequences. */
	cpu.exclusive_address = ~std::uint64_t{0};
	cpu.exclusive_key = 0;
	cpu.exclusive_value = 0;
	cpu.exclusive_value2 = 0;
	cpu.exclusive_version = 0;
	cpu.exclusive_size = 0;
	cpu.pc += 4;
	return continue_after(address, instruction);
}

RunResult Dispatcher::cache_maintenance(CpuState &cpu, GuestMemory &memory,
					std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto operation = instruction & 0xffff'ffe0U;

	if (operation == 0xd50b'7420U) {
		/* DC ZVA ignores the low BlockSize bits. Keep the interpreter
		 * deliberately explicit: validate and commit the complete block
		 * through GuestMemory so an invalid mapping cannot expose a partial
		 * architectural store.
		 */
		const auto block = cpu.read_register(
			instruction & 0x1fU, false) & ~std::uint64_t{63};
		const std::array<std::byte, 64> zeros{};
		if (!memory.write(block, zeros))
			return {StopReason::memory_fault, block, instruction, 0, true};
		cpu.pc += 4;
		return continue_after(address, instruction);
	}

	/* Guest data writes already update the single coherent physical backing.
	 * DC CVAU and DC CIVAC therefore need only an ordering edge. IC IVAU
	 * additionally invalidates translated instruction blocks so self-modifying
	 * code becomes visible at the same architectural synchronization point as
	 * on arm64.
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

} // namespace uml::cpu::arm64
