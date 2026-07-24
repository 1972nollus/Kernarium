// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace arm64 = uml::backend::arm64;
namespace gadgets = arm64::gadgets;

extern "C" void uml_arm64_gadget_link_exit();

#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
namespace {

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

std::uintptr_t gadget_address(void (*gadget)())
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

gadgets::BuildResult build_target(std::uint64_t pc, std::uint16_t value)
{
	const std::array instructions{
		0xd280'0000U | (static_cast<std::uint32_t>(value) << 5),
		0xd400'0001U,
	};
	return gadgets::Builder{}.build(pc, instructions);
}

} // namespace
#endif

int main()
{
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	return 0;
#else
	const std::array<std::uintptr_t, 1> exit_stream{
		gadget_address(uml_arm64_gadget_link_exit)};

	/* An unresolved direct edge selects the data-only fallback stream and
	 * returns the target PC without executing arbitrary memory. */
	{
		gadgets::BlockLink target{
			.stream = exit_stream.data(),
			.instruction_count = 0,
			.guest_pc = 0x4040,
		};
		const std::array instructions{0x1400'0010U}; // b 0x4040
		auto built = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t pc) -> const gadgets::BlockLink * {
				require(pc == target.guest_pc);
				return &target;
			});
		arm64::CpuState cpu{};
		arm64::gadgets::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(built.program, cpu, tlb, executed) ==
			gadgets::Event::continued);
		require(cpu.pc == 0x4040);
		require(executed == 1);
	}

	/* A resolved edge enters the successor stream without returning through
	 * C++; its terminal event and combined instruction count are preserved. */
	{
		auto successor = build_target(0x4040, 42);
		gadgets::BlockLink target{
			.stream = successor.program.words().data(),
			.instruction_count = successor.program.instruction_count(),
			.guest_pc = 0x4040,
		};
		const std::array instructions{0x1400'0010U};
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &target; });
		arm64::CpuState cpu{};
		arm64::gadgets::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			gadgets::Event::system_call);
		require(cpu.registers[0] == 42);
		require(cpu.pc == 0x4048);
		require(executed == 3);
	}

	/* B.cond selects target links directly. Exercise both NZCV outcomes so a
	 * raw guest address can never be mistaken for a host stream pointer. */
	for (const bool taken : {false, true}) {
		auto branch_target = build_target(0x4040, 7);
		auto fallthrough = build_target(0x4004, 9);
		gadgets::BlockLink target{
			.stream = branch_target.program.words().data(),
			.instruction_count =
				branch_target.program.instruction_count(),
			.guest_pc = 0x4040,
		};
		gadgets::BlockLink next{
			.stream = fallthrough.program.words().data(),
			.instruction_count = fallthrough.program.instruction_count(),
			.guest_pc = 0x4004,
		};
		const std::array instructions{0x5400'0200U}; // b.eq 0x4040
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t pc) {
				return pc == target.guest_pc ? &target : &next;
			});
		arm64::CpuState cpu{};
		cpu.nzcv = taken ? 0x4000'0000U : 0;
		arm64::gadgets::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			gadgets::Event::system_call);
		require(cpu.registers[0] == (taken ? 7U : 9U));
		require(executed == 3);
	}

	/* A self-linked hot loop is forced out at the 4K chain boundary so the
	 * outer dispatcher can observe interrupts and enforce its 64K budget. */
	{
		gadgets::BlockLink self{
			.stream = exit_stream.data(),
			.instruction_count = 1,
			.guest_pc = 0x4000,
		};
		const std::array instructions{0x1400'0000U}; // b .
		auto loop = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &self; });
		self.stream = loop.program.words().data();
		arm64::CpuState cpu{};
		arm64::gadgets::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(loop.program, cpu, tlb, executed) ==
			gadgets::Event::continued);
		require(cpu.pc == 0x4000);
		require(executed == 4096);
	}

	return 0;
#endif
}
