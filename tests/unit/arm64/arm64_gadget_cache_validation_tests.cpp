// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"

#include "cpu/arm64/gadgets/executor.hpp"

#include <cstdint>
#include <cstring>

#ifndef UML_GADGET_EXACT_CACHE_VALIDATION
#error "gadget cache validation test needs the configured selector"
#endif

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::emulator::tests::require;
using uml::memory::StaticGuestMemory;

namespace {

constexpr std::uint64_t code_base = 0xd0'0000;
constexpr std::uint32_t system_call = 0xd400'0001U;

struct Outcome final {
	::uml::cpu::EngineEvent event;
	std::size_t executed;
};

[[nodiscard]] Outcome run_once(gadgets::GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, arm64::CpuState &cpu)
{
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto event = executor.run_block(cpu, memory, generation, executed);
	executor.end_execution();
	return {event, executed};
}

[[nodiscard]] constexpr std::uint32_t move_immediate(
	unsigned reg, unsigned value) noexcept
{
	return 0xd280'0000U | ((value & 0xffffU) << 5U) | (reg & 31U);
}

[[nodiscard]] std::uint32_t branch(
	std::uint64_t source, std::uint64_t target)
{
	const auto displacement = static_cast<std::int64_t>(target) -
		static_cast<std::int64_t>(source);
	require((displacement & 3) == 0);
	return 0x1400'0000U |
		(static_cast<std::uint32_t>(displacement >> 2) & 0x03ff'ffffU);
}

void write_mapped32(unsigned long generation, std::uint64_t address,
	std::uint32_t value)
{
	void *destination = nullptr;
	require(darwin_uml_static_vm_translate(
		generation, address, DARWIN_UML_STATIC_WRITE, &destination) ==
		DARWIN_UML_STATIC_TRANSLATED);
	std::memcpy(destination, &value, sizeof(value));
}

void unchanged_words_avoid_retranslation()
{
	constexpr unsigned long generation = 920;
	JitBacking backing{2};
	backing.put32(0, move_immediate(0, 7));
	backing.put32(4, system_call);
	JitMapping mapping{generation, code_base, backing, 2};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 7);

	darwin_uml_static_vm_invalidate_code(
		code_base + DARWIN_UML_GUEST_PAGE_SIZE);
	cpu = {};
	cpu.pc = code_base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 7);
	const auto diagnostics = executor.cache_diagnostics();
#if UML_GADGET_EXACT_CACHE_VALIDATION
	require(diagnostics.translations == 1);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_hits == 1);
#else
	require(diagnostics.translations == 2);
	require(diagnostics.validation_attempts == 0);
#endif
}

void changed_words_rebuild_and_republish_links()
{
	constexpr unsigned long generation = 921;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, branch(code_base, target));
	backing.put32(target - code_base, move_immediate(0, 1));
	backing.put32(target - code_base + 4, system_call);
	JitMapping mapping{generation, code_base, backing, 1};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;

	arm64::CpuState cpu{};
	cpu.pc = target;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	cpu = {};
	cpu.pc = code_base;
	const auto linked = run_once(executor, memory, generation, cpu);
	require(linked.event == ::uml::cpu::EngineEvent::system_call);
	require(linked.executed == 3);
	require(cpu.registers[0] == 1);

	write_mapped32(generation, target, move_immediate(0, 2));
	darwin_uml_static_vm_invalidate_code(target);
	cpu = {};
	cpu.pc = code_base;
	const auto reset_link = run_once(executor, memory, generation, cpu);
	require(reset_link.event == ::uml::cpu::EngineEvent::continued);
	require(reset_link.executed == 1);
	require(cpu.pc == target);

	const auto rebuilt = run_once(executor, memory, generation, cpu);
	require(rebuilt.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 2);
	cpu = {};
	cpu.pc = code_base;
	const auto relinked = run_once(executor, memory, generation, cpu);
	require(relinked.event == ::uml::cpu::EngineEvent::system_call);
	require(relinked.executed == 3);
	require(cpu.registers[0] == 2);

	const auto diagnostics = executor.cache_diagnostics();
#if UML_GADGET_EXACT_CACHE_VALIDATION
	require(diagnostics.translations == 3);
	require(diagnostics.validation_attempts == 2);
	require(diagnostics.validation_hits == 1);
	require(diagnostics.validation_mismatches == 1);
#else
	require(diagnostics.translations == 4);
	require(diagnostics.validation_attempts == 0);
#endif
}

} // namespace

int main()
{
	unchanged_words_avoid_retranslation();
	changed_words_rebuild_and_republish_links();
	return 0;
}
