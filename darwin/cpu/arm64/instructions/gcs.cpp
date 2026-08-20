// SPDX-License-Identifier: GPL-2.0-or-later
#include "../dispatcher.hpp"

#include "../detail/gcs.hpp"
#include "../detail/instruction_helpers.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::cpu::arm64 {
namespace {

using detail::GcsOperation;

[[nodiscard]] RunResult illegal(const CpuState &cpu,
	std::uint32_t instruction) noexcept
{
	return {StopReason::illegal_instruction, cpu.pc, instruction, 0, false};
}

[[nodiscard]] RunResult gcs_fault(const CpuState &cpu,
	std::uint32_t instruction) noexcept
{
	return {StopReason::gcs_fault, cpu.pc, instruction, 0, false};
}

[[nodiscard]] RunResult memory_fault(std::uint32_t instruction,
	std::uint64_t address, bool write) noexcept
{
	return {StopReason::memory_fault, address, instruction, 0, write};
}

[[nodiscard]] bool load_u64(GuestMemory &memory, std::uint64_t address,
	std::uint64_t &value)
{
	std::array<std::byte, 8> bytes{};
	if (!memory.read(address, bytes))
		return false;
	value = detail::decode_little_endian(bytes);
	return true;
}

[[nodiscard]] bool store_u64(GuestMemory &memory, std::uint64_t address,
	std::uint64_t value)
{
	std::array<std::byte, 8> bytes{};
	detail::encode_little_endian(value, bytes);
	return memory.write(address, bytes);
}

[[nodiscard]] bool compare_exchange_u64(GuestMemory &memory,
	std::uint64_t address, std::uint64_t compare, std::uint64_t replacement,
	std::uint64_t &observed)
{
	if ((address & 7U) != 0)
		return false;

	auto *pointer = memory.atomic_pointer(address, 8, MemoryAccess::write);
	if (pointer != nullptr &&
	    (reinterpret_cast<std::uintptr_t>(pointer) & 7U) == 0) {
		auto &storage = *reinterpret_cast<std::uint64_t *>(pointer);
		auto expected = compare;
		(void)std::atomic_ref<std::uint64_t>{storage}
			.compare_exchange_strong(expected, replacement,
				std::memory_order_relaxed, std::memory_order_relaxed);
		observed = expected;
		return true;
	}

	std::array<std::byte, 8> bytes{};
	if (!memory.read(address, bytes))
		return false;
	observed = detail::decode_little_endian(bytes);
	if (observed == compare)
		detail::encode_little_endian(replacement, bytes);
	/* An atomic read-modify-write still validates write permission when its
	 * comparison fails.  Writing the observed value gives fallback memories
	 * that validation without changing the architectural result. */
	return memory.write(address, bytes);
}

[[nodiscard]] RunResult finish(CpuState &cpu, std::uint64_t pc,
	std::uint32_t instruction) noexcept
{
	cpu.pc = pc + 4;
	return detail::continue_after(pc, instruction);
}

} // namespace

RunResult Dispatcher::guarded_control_stack(CpuState &cpu,
	GuestMemory &memory, std::uint32_t instruction) const
{
	const auto decoded = detail::decode_gcs(instruction);
	if (!decoded)
		return illegal(cpu, instruction);

	/* These exception-entry/return aliases are Undefined at EL0.  CpuState is
	 * deliberately an EL0-only model, so diagnose them before observing any
	 * feature bit, register, or memory. */
	if (detail::gcs_is_privileged_exception_operation(decoded.operation))
		return illegal(cpu, instruction);
	if (!cpu.gcs_implemented)
		return illegal(cpu, instruction);

	const auto pc = cpu.pc;
	switch (decoded.operation) {
	case GcsOperation::pop_main: {
		const auto address = cpu.gcs_pointer;
		std::uint64_t value;
		if (!load_u64(memory, address, value))
			return memory_fault(instruction, address, false);
		if ((value & 3U) != 0)
			return gcs_fault(cpu, instruction);
		cpu.gcs_pointer = (address + 8) & ~std::uint64_t{7};
		cpu.write_register(decoded.transfer, value, false);
		return finish(cpu, pc, instruction);
	}
	case GcsOperation::push_main: {
		if (!cpu.gcs_push_enabled)
			return illegal(cpu, instruction);
		const auto address = cpu.gcs_pointer - 8;
		const auto value = cpu.read_register(decoded.transfer, false);
		if (!store_u64(memory, address, value))
			return memory_fault(instruction, address, true);
		cpu.gcs_pointer = address & ~std::uint64_t{7};
		return finish(cpu, pc, instruction);
	}
	case GcsOperation::switch_stack_1: {
		const auto incoming =
			cpu.read_register(decoded.transfer, false);
		if ((incoming & 7U) != 0)
			return memory_fault(instruction, incoming, true);
		const auto compare =
			(incoming & ~std::uint64_t{0xfff}) | 1U;
		const auto replacement =
			(cpu.gcs_pointer & ~std::uint64_t{7}) | 5U;
		std::uint64_t observed;
		if (!compare_exchange_u64(memory, incoming, compare, replacement,
					 observed))
			return memory_fault(instruction, incoming, true);
		if (observed != compare)
			return gcs_fault(cpu, instruction);
		cpu.gcs_pointer = incoming & ~std::uint64_t{7};
		return finish(cpu, pc, instruction);
	}
	case GcsOperation::switch_stack_2: {
		const auto incoming = cpu.gcs_pointer;
		std::uint64_t outgoing_value;
		if (!load_u64(memory, incoming, outgoing_value))
			return memory_fault(instruction, incoming, false);
		if ((outgoing_value & 7U) != 5U)
			return gcs_fault(cpu, instruction);
		const auto outgoing =
			(outgoing_value & ~std::uint64_t{7}) - 8;
		const auto replacement =
			(outgoing & ~std::uint64_t{0xfff}) | 1U;
		if (!store_u64(memory, outgoing, replacement))
			return memory_fault(instruction, outgoing, true);
		cpu.gcs_pointer = (incoming + 8) & ~std::uint64_t{7};
		std::atomic_thread_fence(std::memory_order_seq_cst);
		cpu.write_register(decoded.transfer, outgoing, false);
		return finish(cpu, pc, instruction);
	}
	case GcsOperation::store:
	case GcsOperation::store_unprivileged: {
		/* CheckGCSSTREnabled precedes address generation and SP alignment. */
		if (!cpu.gcs_store_enabled)
			return gcs_fault(cpu, instruction);
		const auto tagged_address = cpu.read_register(decoded.base, true);
		const auto address = cpu.untag_data_address(
			tagged_address, decoded.base == 31U);
		if (decoded.base == 31U && (address & 15U) != 0)
			return memory_fault(instruction, address, true);
		const auto value = cpu.read_register(decoded.transfer, false);
		if (!store_u64(memory, address, value))
			return memory_fault(instruction, address, true);
		return finish(cpu, pc, instruction);
	}
	case GcsOperation::none:
	case GcsOperation::pop_exception_context:
	case GcsOperation::pop_exception:
	case GcsOperation::push_exception:
		break;
	}
	return illegal(cpu, instruction);
}

} // namespace uml::cpu::arm64
