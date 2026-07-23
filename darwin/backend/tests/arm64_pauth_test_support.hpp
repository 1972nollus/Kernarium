// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>

namespace arm64_pauth_test {

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

constexpr std::uint64_t pc = 0x4d0'0000;

class NullMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t, std::span<std::byte>) override
	{
		return false;
	}
	[[nodiscard]] bool write(
		std::uint64_t, std::span<const std::byte>) override
	{
		return false;
	}
};

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

[[nodiscard]] constexpr std::uint32_t data_processing(
	unsigned operation, unsigned destination, unsigned modifier = 31)
{
	return 0xdac1'0000U | ((operation & 15U) << 10) |
		((modifier & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t hint(unsigned immediate)
{
	return 0xd503'201fU | ((immediate & 0x7fU) << 5);
}

[[nodiscard]] constexpr std::uint64_t stripped(
	std::uint64_t source, bool data)
{
	const bool upper = (source & (std::uint64_t{1} << 55)) != 0;
	if (data && !upper)
		return source & 0xff00'7fff'ffff'ffffULL;
	return (source & 0x0000'7fff'ffff'ffffULL) |
		(upper ? 0xffff'8000'0000'0000ULL : 0);
}

[[nodiscard]] inline arm64::CpuState state(
	std::uint64_t value, unsigned destination,
	unsigned modifier = 0, std::uint64_t context = 0x7654'3210'fedc'ba98ULL)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(index) * 0x0101'0101'0101'0101ULL);
	cpu.sp = context;
	if (modifier < cpu.registers.size())
		cpu.registers[modifier] = context;
	if (destination < cpu.registers.size())
		cpu.registers[destination] = value;
	cpu.nzcv = 0xa000'0000U;
	cpu.tls = 0x0123'4567'89ab'cdefULL;
	return cpu;
}

struct ExecutionPair final {
	arm64::CpuState interpreted;
	arm64::CpuState gadget;
};

[[nodiscard]] inline ExecutionPair execute_both(
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	NullMemory memory;
	auto interpreted = initial;
	interpreted.pc = pc;
	const auto result = arm64::Dispatcher{}.execute(
		interpreted, memory, instruction);
	require(result.reason == arm64::StopReason::continued);
	require(interpreted.pc == pc + 4);

	const std::array stream{instruction, 0xd400'0001U};
	const auto built = gadgets::Builder{}.build(pc, stream);
	require(built.complete);
	auto gadget = initial;
	gadget.pc = pc;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::system_call);
	require(gadget.pc == pc + 8);
	require(interpreted.registers == gadget.registers);
	require(interpreted.sp == gadget.sp);
	require(interpreted.nzcv == gadget.nzcv);
	require(interpreted.tls == gadget.tls);
	return {interpreted, gadget};
}

[[nodiscard]] inline ExecutionPair execute_control_both(
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	NullMemory memory;
	auto interpreted = initial;
	interpreted.pc = pc;
	const auto interpreted_result = arm64::Dispatcher{}.execute(
		interpreted, memory, instruction);
	if (interpreted_result.reason != arm64::StopReason::continued)
		std::fprintf(stderr, "control instruction %08x stopped with %u\n",
			instruction, static_cast<unsigned>(interpreted_result.reason));
	require(interpreted_result.reason == arm64::StopReason::continued);

	const std::array stream{instruction};
	const auto built = gadgets::Builder{}.build(pc, stream);
	require(built.complete);
	auto gadget = initial;
	gadget.pc = pc;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::continued);
	require(interpreted.pc == gadget.pc);
	require(interpreted.registers == gadget.registers);
	require(interpreted.sp == gadget.sp);
	require(interpreted.nzcv == gadget.nzcv);
	return {interpreted, gadget};
}

inline void require_only_destination_changed(
	const arm64::CpuState &before, const arm64::CpuState &after,
	unsigned destination)
{
	for (unsigned index = 0; index < before.registers.size(); ++index)
		if (index != destination)
			require(after.registers[index] == before.registers[index]);
	require(after.sp == before.sp);
	require(after.nzcv == before.nzcv);
	require(after.tls == before.tls);
}

inline void require_illegal(std::uint32_t instruction)
{
	NullMemory memory;
	const auto initial = state(0x1234'5678'9abcULL, 3, 7);
	auto interpreted = initial;
	interpreted.pc = pc;
	require(arm64::Dispatcher{}.execute(
		interpreted, memory, instruction).reason ==
		arm64::StopReason::illegal_instruction);
	require(interpreted.pc == pc);
	require(interpreted.registers == initial.registers);
	require(interpreted.sp == initial.sp);

	const std::array stream{instruction, 0xd400'0001U};
	const auto built = gadgets::Builder{}.build(pc, stream);
	require(built.complete);
	auto gadget = initial;
	gadget.pc = pc;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::illegal_instruction);
	require(gadget.pc == pc);
	require(gadget.registers == initial.registers);
	require(gadget.sp == initial.sp);
}

} // namespace arm64_pauth_test
