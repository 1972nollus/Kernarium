// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>
#include <vector>

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

class CountingMemory final : public arm64::GuestMemory {
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

void require(bool condition,
	     const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

constexpr std::uint64_t base = 0x4'0000;

[[nodiscard]] arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0xffff'0000'1234'0000ULL | index;
	cpu.sp = 0;
	cpu.nzcv = 0xd000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	return cpu;
}

void require_state(const arm64::CpuState &actual,
		   const arm64::CpuState &initial, std::uint64_t pc)
{
	require(actual.registers == initial.registers);
	require(actual.sp == initial.sp);
	require(actual.pc == pc);
	require(actual.nzcv == initial.nzcv);
	require(actual.fpcr == initial.fpcr);
	require(actual.fpsr == initial.fpsr);
	require(actual.tls == initial.tls);
}

[[nodiscard]] constexpr std::uint32_t immediate(
	unsigned offset, unsigned rn, unsigned operation)
{
	return 0xf980'0000U | (offset << 10) | (rn << 5) | operation;
}

[[nodiscard]] constexpr std::uint32_t literal(
	unsigned offset, unsigned operation)
{
	return 0xd800'0000U | (offset << 5) | operation;
}

[[nodiscard]] constexpr std::uint32_t register_offset(unsigned rm,
	unsigned option, bool scaled, unsigned rn, unsigned operation)
{
	return 0xf8a0'0800U | (rm << 16) | (option << 13) |
		(static_cast<unsigned>(scaled) << 12) | (rn << 5) | operation;
}

[[nodiscard]] constexpr std::uint32_t unscaled(
	unsigned offset, unsigned rn, unsigned operation, unsigned mode = 0)
{
	return 0xf880'0000U | (offset << 12) | (mode << 10) |
		(rn << 5) | operation;
}

void append_valid_encodings(std::vector<std::uint32_t> &stream)
{
	for (unsigned offset = 0; offset < 4096; ++offset)
		for (unsigned operation = 0; operation < 32; ++operation)
			stream.push_back(immediate(offset, 31, operation));
	for (unsigned rn = 0; rn < 32; ++rn)
		for (unsigned operation = 0; operation < 32; ++operation)
			stream.push_back(immediate(0, rn, operation));

	for (unsigned offset = 0; offset < (1U << 19); ++offset)
		stream.push_back(literal(offset, 31));
	for (unsigned operation = 0; operation < 32; ++operation)
		stream.push_back(literal(0, operation));

	for (const unsigned option : {2U, 3U, 6U, 7U})
		for (const bool scaled : {false, true})
			for (unsigned rm = 0; rm < 32; ++rm)
				for (unsigned rn = 0; rn < 32; ++rn)
					for (unsigned operation = 0; operation < 32;
					     ++operation)
						stream.push_back(register_offset(
							rm, option, scaled, rn, operation));

	for (unsigned offset = 0; offset < 512; ++offset)
		for (unsigned rn = 0; rn < 32; ++rn)
			for (unsigned operation = 0; operation < 32; ++operation)
				stream.push_back(unscaled(offset, rn, operation));
}

void valid_prefetches()
{
	std::vector<std::uint32_t> stream;
	stream.reserve(1'500'000);
	append_valid_encodings(stream);

	CountingMemory memory;
	const auto initial = state();
	auto interpreted = initial;
	interpreted.pc = base;
	for (const auto instruction : stream)
		require(arm64::Dispatcher{}.execute(
			interpreted, memory, instruction).reason ==
			arm64::StopReason::continued);
	require(memory.reads == 0);
	require(memory.writes == 0);
	require_state(
		interpreted, initial, base + stream.size() * sizeof(std::uint32_t));

	stream.push_back(0xd400'0001U);
	const auto built = gadgets::Builder{}.build(base, stream);
	require(built.complete);
	require(built.program.instruction_count() == stream.size());
	auto gadget = initial;
	gadget.pc = base;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::system_call);
	require_state(
		gadget, initial, base + stream.size() * sizeof(std::uint32_t));
}

void reserved_register_offsets()
{
	CountingMemory memory;
	for (const unsigned option : {0U, 1U, 4U, 5U})
		for (const bool scaled : {false, true})
			for (unsigned rm = 0; rm < 32; ++rm)
				for (unsigned rn = 0; rn < 32; ++rn)
					for (unsigned operation = 0; operation < 32;
					     ++operation) {
						const auto instruction = register_offset(
							rm, option, scaled, rn, operation);
						auto cpu = state();
						cpu.pc = base;
						require(arm64::Dispatcher{}.execute(
							cpu, memory, instruction).reason ==
							arm64::StopReason::illegal_instruction);
						require(cpu.pc == base);
					}
	for (const unsigned mode : {1U, 3U})
		for (unsigned offset = 0; offset < 512; ++offset) {
			const auto instruction = unscaled(offset, 1, 2, mode);
			auto cpu = state();
			cpu.pc = base;
			require(arm64::Dispatcher{}.execute(
				cpu, memory, instruction).reason ==
				arm64::StopReason::illegal_instruction);
		}
	require(memory.reads == 0);
	require(memory.writes == 0);

	auto tlb = std::make_unique<gadgets::Tlb>();
	for (const unsigned option : {0U, 1U, 4U, 5U}) {
		const auto instruction = register_offset(31, option, true, 31, 31);
		const auto built = gadgets::Builder{}.build(
			base, std::array{instruction});
		require(built.complete);
		auto cpu = state();
		cpu.pc = base;
		tlb->prepare(0, darwin_uml_static_vm_changes(0));
		require(gadgets::execute(built.program, cpu, *tlb) ==
			gadgets::Event::illegal_instruction);
		require(cpu.pc == base);
	}
}

} // namespace

int main()
{
	valid_prefetches();
	reserved_register_offsets();
	return 0;
}
