// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"

#include "cpu/engine.hpp"
#include "cpu/engine_factory.hpp"
#include "memory/static_guest_memory.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <signal.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace {

using ::uml::cpu::EngineKind;
using ::uml::emulator::tests::JitBacking;
using ::uml::emulator::tests::JitMapping;

constexpr std::uint64_t code_base = 0x5d00'0000;
constexpr std::uint64_t carried_source = code_base + 0x40;
constexpr std::uint32_t indirect_branch_x0 = 0xd61f'0000U;
constexpr std::uint32_t system_call = 0xd400'0001U;

[[noreturn]] void fail(const char *message)
{
	std::fprintf(stderr, "arm64-pc-alignment-contract: %s\n", message);
	std::_Exit(EXIT_FAILURE);
}

void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

[[nodiscard]] EngineKind parse_engine(std::string_view name)
{
	if (name == "interpreter")
		return EngineKind::interpreter;
	if (name == "gadgets")
		return EngineKind::gadgets;
	if (name == "jit")
		return EngineKind::jit;
	fail("unknown child engine");
}

[[nodiscard]] const char *engine_name(EngineKind kind) noexcept
{
	switch (kind) {
	case EngineKind::interpreter:
		return "interpreter";
	case EngineKind::gadgets:
		return "gadgets";
	case EngineKind::jit:
		return "jit";
	}
	return "unknown";
}

using EntryPoint = int (*)(void *, void *, unsigned long, unsigned long,
	unsigned long *);

[[nodiscard]] EntryPoint bridge_for(EngineKind kind) noexcept
{
	switch (kind) {
	case EngineKind::interpreter:
		return &darwin_uml_interpreter_enter_guest;
	case EngineKind::gadgets:
		return &darwin_uml_static_enter_guest;
	case EngineKind::jit:
		return &darwin_uml_jit_enter_guest;
	}
	return nullptr;
}

[[nodiscard]] ::uml::cpu::ExecutionResult run(
	::uml::cpu::CpuEngine &engine, ::uml::cpu::arm64::CpuState &cpu,
	::uml::memory::StaticGuestMemory &memory, unsigned long generation)
{
	::uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = generation,
		.instruction_budget = 8,
	};
	return engine.run(request);
}

void require_alignment_result(
	const ::uml::cpu::ExecutionResult &result,
	const ::uml::cpu::arm64::CpuState &cpu, std::uint64_t address,
	std::size_t retired)
{
	require(result.reason == ::uml::cpu::ExitReason::pc_alignment,
		"engine did not report a PC-alignment exit");
	require(result.retired_instructions == retired,
		"PC-alignment exit lost its precise retired prefix");
	require(cpu.pc == address,
		"PC-alignment exit changed the faulting instruction address");
	require(result.fault.address == address,
		"PC-alignment exit reported the wrong address");
	require(result.fault.access == ::uml::memory::MemoryAccess::execute,
		"PC-alignment exit lost its instruction-access direction");
}

void direct_engine_contract(EngineKind kind, unsigned long generation)
{
	JitBacking backing{1};
	backing.put32(0, indirect_branch_x0);
	backing.put32(4, system_call);
	backing.put32(carried_source - code_base,
		0x9100'0508U); // add x8, x8, #1
	backing.put32(carried_source - code_base + 4,
		0x9100'0929U); // add x9, x9, #2
	backing.put32(carried_source - code_base + 8,
		indirect_branch_x0);
	JitMapping mapping{generation, code_base, backing, 1,
		PROT_READ | PROT_EXEC};
	::uml::memory::StaticGuestMemory memory;
	memory.set_generation(generation);
	auto engine = ::uml::cpu::make_cpu_engine(kind);
	require(engine != nullptr && engine->kind() == kind,
		"factory substituted a different engine");

	::uml::cpu::arm64::CpuState cpu;
	cpu.pc = code_base + 2;
	const auto entry = run(*engine, cpu, memory, generation);
	require_alignment_result(entry, cpu, code_base + 2, 0);

	/* Reuse the exact engine after the exceptional entry. */
	cpu.pc = code_base + 4;
	const auto entry_retry = run(*engine, cpu, memory, generation);
	require(entry_retry.reason == ::uml::cpu::ExitReason::system_call &&
		entry_retry.retired_instructions == 1 &&
		cpu.pc == code_base + 8,
		"engine was not reusable after a misaligned entry");

	/* The indirect transfer retires, then the next-instruction alignment
	 * check reports its target without fetching from that address. */
	cpu = {};
	cpu.pc = code_base;
	cpu.registers[0] = code_base + 2;
	const auto branch = run(*engine, cpu, memory, generation);
	require_alignment_result(branch, cpu, code_base + 2, 1);

	cpu = {};
	cpu.pc = code_base;
	cpu.registers[0] = code_base + 4;
	const auto branch_retry = run(*engine, cpu, memory, generation);
	require(branch_retry.reason == ::uml::cpu::ExitReason::system_call &&
		branch_retry.retired_instructions == 2 &&
		cpu.pc == code_base + 8,
		"engine cache/link state was not reusable after a bad target");

	if (kind == EngineKind::jit) {
		/* Immediate branches cannot create a misaligned AArch64 PC. A BR miss
		 * therefore owns the only carried-predecessor path to the common outer
		 * alignment check, which must observe its canonicalized x8/x9 prefix.
		 */
		cpu = {};
		cpu.pc = carried_source;
		cpu.registers[0] = code_base + 2;
		cpu.registers[8] = 10;
		cpu.registers[9] = 20;
		const auto carried = run(*engine, cpu, memory, generation);
		require_alignment_result(carried, cpu, code_base + 2, 3);
		require(cpu.registers[8] == 11 && cpu.registers[9] == 22,
			"PC-alignment exit lost carried x8/x9 state");
	}
}

void bridge_contract(EngineKind kind, unsigned long generation)
{
	JitBacking backing{1};
	backing.put32(0, indirect_branch_x0);
	backing.put32(4, system_call);
	backing.put32(carried_source - code_base,
		0x9100'0508U); // add x8, x8, #1
	backing.put32(carried_source - code_base + 4,
		0x9100'0929U); // add x9, x9, #2
	backing.put32(carried_source - code_base + 8,
		indirect_branch_x0);
	JitMapping mapping{generation, code_base, backing, 1,
		PROT_READ | PROT_EXEC};
	const auto enter = bridge_for(kind);
	require(enter != nullptr, "engine bridge is unavailable");
	darwin_uml_guest_fp_registers fp{};
	unsigned long tls = 0;

	darwin_uml_guest_registers registers{};
	registers.pc = code_base + 2;
	const auto entry = enter(&registers, &fp, sizeof(fp), generation, &tls);
	require(entry == DARWIN_UML_EVENT_PC_ALIGNMENT,
		"C bridge did not export the PC-alignment event");
	require(registers.pc == code_base + 2 &&
		registers.fault.address == code_base + 2 &&
		registers.fault.error_code == 0 &&
		registers.fault.trap_number == 0,
		"C bridge changed PC-alignment metadata");

	registers.pc = code_base + 4;
	const auto entry_retry = enter(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(entry_retry == DARWIN_UML_EVENT_SYSCALL &&
		registers.pc == code_base + 8,
		"C bridge was not reusable after a misaligned entry");

	registers = {};
	registers.pc = code_base;
	registers.x[0] = code_base + 2;
	const auto branch = enter(&registers, &fp, sizeof(fp), generation, &tls);
	require(branch == DARWIN_UML_EVENT_PC_ALIGNMENT &&
		registers.pc == code_base + 2 &&
		registers.fault.address == code_base + 2,
		"C bridge lost a misaligned indirect target");

	registers = {};
	registers.pc = code_base;
	registers.x[0] = code_base + 4;
	const auto branch_retry = enter(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(branch_retry == DARWIN_UML_EVENT_SYSCALL &&
		registers.pc == code_base + 8,
		"C bridge cache/link state was not reusable after a bad target");

	if (kind == EngineKind::jit) {
		registers = {};
		registers.pc = carried_source;
		registers.x[0] = code_base + 2;
		registers.x[8] = 30;
		registers.x[9] = 40;
		const auto carried = enter(
			&registers, &fp, sizeof(fp), generation, &tls);
		require(carried == DARWIN_UML_EVENT_PC_ALIGNMENT &&
			registers.pc == code_base + 2 &&
			registers.fault.address == code_base + 2 &&
			registers.x[8] == 31 && registers.x[9] == 42,
			"C bridge lost carried state at a PC-alignment exit");
	}
}

int run_child(EngineKind kind)
{
	if (!::uml::cpu::cpu_engine_available(kind))
		return 77;
	const auto ordinal = static_cast<unsigned long>(kind);
	direct_engine_contract(kind, 0x8d00 + ordinal * 2);
	bridge_contract(kind, 0x8d01 + ordinal * 2);
	return EXIT_SUCCESS;
}

void run_subprocess(const char *executable, EngineKind kind)
{
	const auto *name = engine_name(kind);
	std::array arguments{
		const_cast<char *>(executable),
		const_cast<char *>("--child"),
		const_cast<char *>(name),
		static_cast<char *>(nullptr),
	};
	pid_t child{};
	const auto spawned = posix_spawnp(
		&child, executable, nullptr, nullptr, arguments.data(), environ);
	require(spawned == 0, "cannot spawn isolated engine child");

	int status{};
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds{20};
	for (;;) {
		const auto waited = waitpid(child, &status, WNOHANG);
		if (waited == child)
			break;
		require(waited == 0 || (waited < 0 && errno == EINTR),
			"cannot wait for isolated engine child");
		if (std::chrono::steady_clock::now() >= deadline) {
			(void)kill(child, SIGKILL);
			(void)waitpid(child, &status, 0);
			fail("isolated engine child timed out");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds{10});
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 77)
		return;
	require(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
		"isolated engine child terminated or failed");
}

} // namespace

int main(int argc, char **argv)
{
	if (argc == 3 && std::string_view{argv[1]} == "--child")
		return run_child(parse_engine(argv[2]));
	require(argc >= 1 && argv[0] != nullptr,
		"test executable path is unavailable");
	for (const auto kind : {EngineKind::interpreter, EngineKind::gadgets,
	     EngineKind::jit})
		run_subprocess(argv[0], kind);
	std::puts("arm64-pc-alignment-contract: passed");
	return EXIT_SUCCESS;
}
