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

namespace arm64_simd_test {

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

inline constexpr std::uint64_t pc = 0x780'0000;

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

class NoMemory final : public arm64::GuestMemory {
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

	std::size_t reads{};
	std::size_t writes{};
};

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed * 67U + reg * 29U + byte * 17U + 3U);
	for (unsigned reg = 0; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(reg + seed) *
			 0x0101'0101'0101'0101ULL);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.pc = pc;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0;
	cpu.fpsr = seed & 1U;
	return cpu;
}

inline void require_architecture_equal(const arm64::CpuState &expected,
	const arm64::CpuState &actual, std::uint32_t instruction = 0U)
{
	require(actual.registers == expected.registers);
	require(actual.sp == expected.sp);
	if (actual.nzcv != expected.nzcv)
		std::fprintf(stderr, "NZCV mismatch: expected=%08x actual=%08x\n",
			expected.nzcv, actual.nzcv);
	require(actual.nzcv == expected.nzcv);
	for (unsigned reg = 0; reg < expected.vectors.size(); ++reg) {
		if (actual.vectors[reg].bytes != expected.vectors[reg].bytes) {
			for (unsigned byte = 0;
			     byte < expected.vectors[reg].bytes.size(); ++byte) {
				if (actual.vectors[reg].bytes[byte] ==
				    expected.vectors[reg].bytes[byte])
					continue;
				std::fprintf(stderr,
					"vector mismatch for %#010x: v%u byte %u "
					"expected=%02x actual=%02x\n", instruction, reg,
					byte,
					std::to_integer<unsigned>(
						expected.vectors[reg].bytes[byte]),
					std::to_integer<unsigned>(
						actual.vectors[reg].bytes[byte]));
				break;
			}
		}
		require(actual.vectors[reg].bytes == expected.vectors[reg].bytes);
	}
	require(actual.fpcr == expected.fpcr);
	require(actual.fpmr == expected.fpmr);
	if (actual.fpsr != expected.fpsr)
		std::fprintf(stderr, "FPSR mismatch: expected=%08x actual=%08x\n",
			expected.fpsr, actual.fpsr);
	require(actual.fpsr == expected.fpsr);
	require(actual.tls == expected.tls);
}

class Harness final {
public:
	Harness() : tlb{std::make_unique<gadgets::Tlb>()}
	{
		tlb->prepare(0, darwin_uml_static_vm_changes(0));
	}

	[[nodiscard]] arm64::CpuState run_interpreter(
		std::uint32_t instruction, const arm64::CpuState &initial)
	{
		NoMemory memory;
		auto result_state = initial;
		result_state.pc = pc;
		const auto result = arm64::Dispatcher{}.execute(
			result_state, memory, instruction);
		if (result.reason != arm64::StopReason::continued)
			std::fprintf(stderr,
				"expected interpreter continuation for %#010x, reason=%u\n",
				instruction, static_cast<unsigned>(result.reason));
		require(result.reason == arm64::StopReason::continued);
		require(result_state.pc == pc + 4);
		require(memory.reads == 0 && memory.writes == 0);
		return result_state;
	}

	void require_differential(std::uint32_t instruction,
		const arm64::CpuState &initial)
	{
		NoMemory memory;
		auto expected = initial;
		expected.pc = pc;
		const auto interpreted = arm64::Dispatcher{}.execute(
			expected, memory, instruction);
		require(interpreted.reason == arm64::StopReason::continued);
		require(expected.pc == pc + 4);
		require(memory.reads == 0 && memory.writes == 0);

		const std::array stream{instruction, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(pc, stream);
		require(built.complete);
		auto actual = initial;
		actual.pc = pc;
		const auto event = gadgets::execute(built.program, actual, *tlb);
		if (event != gadgets::Event::system_call) {
			std::fprintf(stderr, "expected gadget syscall for %#010x, event=%u\n",
				instruction, static_cast<unsigned>(event));
			for (const auto word : built.program.words())
				std::fprintf(stderr, "  program word: %#018lx\n",
					static_cast<unsigned long>(word));
		}
		require(event == gadgets::Event::system_call);
		require(actual.pc == pc + 8);
		expected.pc = actual.pc;
		require_architecture_equal(expected, actual, instruction);
	}

	void require_illegal(std::uint32_t instruction,
		const arm64::CpuState &initial = state())
	{
		NoMemory memory;
		auto interpreted = initial;
		interpreted.pc = pc;
		const auto before = interpreted;
		const auto result = arm64::Dispatcher{}.execute(
			interpreted, memory, instruction);
		if (result.reason != arm64::StopReason::illegal_instruction)
			std::fprintf(stderr,
				"expected interpreter illegal for %#010x, reason=%u\n",
				instruction, static_cast<unsigned>(result.reason));
		require(result.reason == arm64::StopReason::illegal_instruction);
		require_architecture_equal(before, interpreted);
		require(interpreted.pc == pc);
		require(memory.reads == 0 && memory.writes == 0);

		const std::array stream{instruction, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(pc, stream);
		require(built.complete);
		auto actual = initial;
		actual.pc = pc;
		const auto actual_before = actual;
		const auto event = gadgets::execute(built.program, actual, *tlb);
		if (event != gadgets::Event::illegal_instruction)
			std::fprintf(stderr,
				"expected gadget illegal for %#010x, event=%u\n",
				instruction, static_cast<unsigned>(event));
		require(event == gadgets::Event::illegal_instruction);
		require_architecture_equal(actual_before, actual, instruction);
		require(actual.pc == pc);
	}

private:
	std::unique_ptr<gadgets::Tlb> tlb;
};

[[nodiscard]] constexpr std::uint32_t three_same(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode, unsigned destination,
	unsigned left, unsigned right)
{
	return (scalar ? 0x5e20'0400U : 0x0e20'0400U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((opcode & 31U) << 11) |
		((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr bool valid_integer_three_same(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode)
{
	if (opcode >= 24U || opcode == 3U)
		return false;
	if (scalar) {
		if (opcode == 1U || opcode == 5U || opcode == 9U || opcode == 11U)
			return true;
		if (opcode == 6U || opcode == 7U || opcode == 8U || opcode == 10U ||
		    opcode == 16U || opcode == 17U)
			return size == 3U;
		return opcode == 22U && (size == 1U || size == 2U);
	}
	if (!q && size == 3U)
		return false;
	if (size == 3U)
		return opcode == 1U || opcode == 5U || opcode == 6U ||
			opcode == 7U || opcode == 8U || opcode == 9U ||
			opcode == 10U || opcode == 11U || opcode == 16U ||
			opcode == 17U || (opcode == 23U && !unsigned_group);
	if (opcode == 19U && unsigned_group)
		return size == 0U;
	if (opcode == 22U)
		return size == 1U || size == 2U;
	if (opcode == 23U)
		return !unsigned_group;
	return true;
}

} // namespace arm64_simd_test
