// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>
#include <vector>

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x4e0'0000;

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

class RejectingMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t,
		std::span<std::byte>) override
	{
		++reads;
		return false;
	}

	[[nodiscard]] bool write(std::uint64_t,
		std::span<const std::byte>) override
	{
		++writes;
		return false;
	}

	unsigned reads{};
	unsigned writes{};
};

[[nodiscard]] constexpr std::uint32_t rprfm(unsigned operation,
	unsigned metadata, unsigned base)
{
	/* operation = option<2>:option<0>:S:Rt<2:0>; option<1> and
	 * Rt<4:3> are the fixed RPRFM allocation bits. */
	const unsigned option = (((operation >> 5) & 1U) << 2) | 2U |
		((operation >> 4) & 1U);
	const unsigned scale = (operation >> 3) & 1U;
	const unsigned rt = 24U | (operation & 7U);
	return 0xf8a0'0800U | ((metadata & 31U) << 16) |
		(option << 13) | (scale << 12) | ((base & 31U) << 5) | rt;
}

[[nodiscard]] arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0x9000'0000'1000'0000ULL |
			(static_cast<std::uint64_t>(index) << 32) | index;
	cpu.sp = 0xffff'ffff'ffff'fff0ULL;
	cpu.nzcv = 0xb000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	return cpu;
}

void require_unchanged(const arm64::CpuState &actual,
	const arm64::CpuState &before, std::uint64_t expected_pc)
{
	require(actual.registers == before.registers && actual.sp == before.sp &&
		actual.pc == expected_pc && actual.nzcv == before.nzcv &&
		actual.fpcr == before.fpcr && actual.fpsr == before.fpsr &&
		actual.tls == before.tls);
}

void encoding_examples()
{
	require(rprfm(0, 3, 2) == 0xf8a3'4858U); // PLDKEEP
	require(rprfm(1, 3, 2) == 0xf8a3'4859U); // PSTKEEP
	require(rprfm(4, 3, 2) == 0xf8a3'485cU); // PLDSTRM
	require(rprfm(5, 3, 2) == 0xf8a3'485dU); // PSTSTRM
	require(rprfm(63, 30, 31) == 0xf8be'fbffU);
}

void exhaustive_operations_registers_and_no_fault_behavior()
{
	std::vector<std::uint32_t> stream;
	stream.reserve(64U * 32U * 32U + 1U);
	RejectingMemory memory;
	const auto before = state();
	auto cpu = before;
	cpu.pc = pc;
	for (unsigned operation = 0; operation < 64; ++operation) {
		for (unsigned metadata = 0; metadata < 32; ++metadata) {
			for (unsigned base = 0; base < 32; ++base) {
				const auto instruction = rprfm(operation, metadata, base);
				stream.push_back(instruction);
				require(arm64::Dispatcher{}.execute(cpu, memory,
					instruction).reason == arm64::StopReason::continued);
			}
		}
	}
	require(memory.reads == 0 && memory.writes == 0);
	require_unchanged(cpu, before,
		pc + stream.size() * sizeof(std::uint32_t));

	stream.push_back(0xd400'0001U);
	const auto built = gadgets::Builder{}.build(pc, stream);
	require(built.complete &&
		built.program.instruction_count() == stream.size());
	auto gadget_cpu = before;
	gadget_cpu.pc = pc;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget_cpu, *tlb) ==
		gadgets::Event::system_call);
	require_unchanged(gadget_cpu, before,
		pc + stream.size() * sizeof(std::uint32_t));
}

void production_is_a_direct_nop_gadget()
{
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	return;
#else
	const auto nop = gadgets::Builder{}.build(pc,
		std::array{0xd503'201fU});
	for (unsigned operation = 0; operation < 64; ++operation) {
		const auto built = gadgets::Builder{}.build(pc,
			std::array{rprfm(operation, 30, 31)});
		require(built.complete && built.program.words().front() ==
			nop.program.words().front());
	}
#endif
}

} // namespace

int main()
{
	encoding_examples();
	exhaustive_operations_registers_and_no_fault_behavior();
	production_is_a_direct_nop_gadget();
}
