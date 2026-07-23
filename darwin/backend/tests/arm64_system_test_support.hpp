// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>
#include <utility>
#include <vector>

namespace arm64_system_test {

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

inline constexpr std::uint64_t pc = 0x6a0'0000;

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

class Memory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t, std::span<std::byte>) override
	{
		++reads;
		return false;
	}

	[[nodiscard]] bool write(
		std::uint64_t, std::span<const std::byte>) override
	{
		++writes;
		return false;
	}

	void invalidate_code(std::uint64_t address) override
	{
		invalidations.push_back(address);
	}

	std::size_t reads{};
	std::size_t writes{};
	std::vector<std::uint64_t> invalidations;
};

[[nodiscard]] inline arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0x9182'7364'5546'3728ULL ^
			(static_cast<std::uint64_t>(index) * 0x0101'0101'0101'0101ULL);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0004'0000U;
	cpu.fpsr = 0x0800'0001U;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.exclusive_address = 0x2000;
	cpu.exclusive_key = 0x3456;
	cpu.exclusive_value = 0x1020'3040'5060'7080ULL;
	cpu.exclusive_value2 = 0x8877'6655'4433'2211ULL;
	cpu.exclusive_version = 17;
	cpu.exclusive_size = 8;
	return cpu;
}

inline void require_unchanged(const arm64::CpuState &before,
	const arm64::CpuState &after)
{
	require(after.registers == before.registers);
	require(after.sp == before.sp && after.pc == before.pc);
	require(after.nzcv == before.nzcv && after.fpcr == before.fpcr &&
		after.fpsr == before.fpsr && after.tls == before.tls);
	require(after.pstate_ssbs == before.pstate_ssbs &&
		after.pstate_dit == before.pstate_dit &&
		after.pstate_tco == before.pstate_tco &&
		after.pstate_sm == before.pstate_sm &&
		after.pstate_za == before.pstate_za &&
		after.pstate_daif == before.pstate_daif);
	require(after.exclusive_address == before.exclusive_address &&
		after.exclusive_key == before.exclusive_key &&
		after.exclusive_value == before.exclusive_value &&
		after.exclusive_value2 == before.exclusive_value2 &&
		after.exclusive_version == before.exclusive_version &&
		after.exclusive_size == before.exclusive_size);
}

class Harness final {
public:
	Harness() : tlb{std::make_unique<gadgets::Tlb>()}
	{
		tlb->prepare(0, darwin_uml_static_vm_changes(0));
	}

	void require_interpreter_illegal(std::uint32_t instruction)
	{
		Memory memory;
		auto cpu = state();
		cpu.pc = pc;
		const auto before = cpu;
		const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
		require(result.reason == arm64::StopReason::illegal_instruction);
		require(result.address == pc && result.instruction == instruction);
		require_unchanged(before, cpu);
		require(memory.reads == 0 && memory.writes == 0 &&
			memory.invalidations.empty());
	}

	void require_gadget_illegal(std::uint32_t instruction)
	{
		const std::array stream{instruction, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(pc, stream);
		require(built.complete);
		auto cpu = state();
		cpu.pc = pc;
		const auto before = cpu;
		require(gadgets::execute(built.program, cpu, *tlb) ==
			gadgets::Event::illegal_instruction);
		require_unchanged(before, cpu);
	}

	void require_illegal(std::uint32_t instruction)
	{
		require_interpreter_illegal(instruction);
		require_gadget_illegal(instruction);
	}

	[[nodiscard]] std::pair<arm64::CpuState, Memory>
	run_interpreter(std::uint32_t instruction)
	{
		Memory memory;
		auto cpu = state();
		cpu.pc = pc;
		const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
		require(result.reason == arm64::StopReason::continued);
		require(cpu.pc == pc + 4);
		return {cpu, std::move(memory)};
	}

	[[nodiscard]] arm64::CpuState run_gadget(std::uint32_t instruction)
	{
		const std::array stream{instruction, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(pc, stream);
		require(built.complete);
		auto cpu = state();
		cpu.pc = pc;
		require(gadgets::execute(built.program, cpu, *tlb) ==
			gadgets::Event::system_call);
		require(cpu.pc == pc + 8);
		return cpu;
	}

private:
	std::unique_ptr<gadgets::Tlb> tlb;
};

[[nodiscard]] constexpr std::uint32_t system_instruction(bool result,
	unsigned op1, unsigned crn, unsigned crm, unsigned op2, unsigned rt)
{
	return (result ? 0xd528'0000U : 0xd508'0000U) |
		((op1 & 7U) << 16) | ((crn & 15U) << 12) |
		((crm & 15U) << 8) | ((op2 & 7U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t system_register(bool read,
	unsigned o0, unsigned op1, unsigned crn, unsigned crm, unsigned op2,
	unsigned rt)
{
	return 0xd510'0000U | (static_cast<unsigned>(read) << 21) |
		((o0 & 1U) << 19) | ((op1 & 7U) << 16) |
		((crn & 15U) << 12) | ((crm & 15U) << 8) |
		((op2 & 7U) << 5) | (rt & 31U);
}

} // namespace arm64_system_test
