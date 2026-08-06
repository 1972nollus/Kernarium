/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/engine.hpp"
#include "cpu/engine_factory.hpp"
#include "cpu/arm64/engine_execution.hpp"
#include "memory/static_guest_memory.hpp"
#include "arm64/mach_vm_protection_test_support.hpp"
#include "arm64/jit_test_support.hpp"
#include "uml_darwin_guest.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>

namespace allocation_injection {
std::atomic_bool fail_next{};
}

void *operator new(std::size_t size)
{
	if (allocation_injection::fail_next.exchange(
			false, std::memory_order_acq_rel))
		throw std::bad_alloc{};
	if (auto *memory = std::malloc(size == 0 ? 1 : size); memory != nullptr)
		return memory;
	throw std::bad_alloc{};
}

void *operator new[](std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void *memory) noexcept
{
	std::free(memory);
}

void operator delete[](void *memory) noexcept
{
	std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
	std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept
{
	std::free(memory);
}

namespace {

class TestMemory final : public uml::cpu::GuestAddressSpace {
public:
	explicit TestMemory(std::uint32_t instruction = 0xd400'0001U)
	{
		std::memcpy(bytes_.data(), &instruction, sizeof(instruction));
	}

	[[nodiscard]] bool read(
		std::uint64_t address, std::span<std::byte> destination) override
	{
		if (address > bytes_.size() ||
			destination.size() > bytes_.size() - address)
			return false;
		std::memcpy(destination.data(), bytes_.data() + address,
			destination.size());
		return true;
	}

	[[nodiscard]] bool write(
		std::uint64_t address,
		std::span<const std::byte> source) override
	{
		if (address > bytes_.size() || source.size() > bytes_.size() - address)
			return false;
		std::memcpy(bytes_.data() + address, source.data(), source.size());
		return true;
	}

private:
	std::array<std::byte, 4096> bytes_{};
};

class TranslatedBoundaryExecutor final {
public:
	[[nodiscard]] bool begin_execution(unsigned long, std::uint64_t)
	{
		++begin_calls;
		return true;
	}

	void end_execution() noexcept
	{
		++end_calls;
	}

	[[nodiscard]] uml::cpu::EngineEvent run_block(
		uml::cpu::arm64::CpuState &, uml::memory::StaticGuestMemory &,
		unsigned long, std::size_t &executed)
	{
		++block_calls;
		executed = block_size;
		return uml::cpu::EngineEvent::interrupt;
	}

	[[nodiscard]] bool preempted() const noexcept
	{
		return false;
	}

	[[nodiscard]] std::uint64_t fault_address() const noexcept
	{
		return 0;
	}

	[[nodiscard]] uml::memory::Access fault_access() const noexcept
	{
		return uml::memory::Access::read;
	}

	[[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept
	{
		return 0;
	}

	static constexpr std::size_t block_size = 4096;
	std::size_t begin_calls{};
	std::size_t end_calls{};
	std::size_t block_calls{};
};

class ThrowingTranslatedExecutor final {
public:
	[[nodiscard]] bool begin_execution(unsigned long, std::uint64_t)
	{
		++begin_calls;
		if (entered)
			return false;
		entered = true;
		return true;
	}

	void end_execution() noexcept
	{
		++end_calls;
		entered = false;
	}

	[[nodiscard]] uml::cpu::EngineEvent run_block(
		uml::cpu::arm64::CpuState &, uml::memory::StaticGuestMemory &,
		unsigned long, std::size_t &executed)
	{
		++block_calls;
		executed = committed_before_throw;
		if (throw_on_run)
			throw std::runtime_error{"injected translated-engine failure"};
		return uml::cpu::EngineEvent::fatal;
	}

	[[nodiscard]] bool preempted() const noexcept { return false; }
	[[nodiscard]] std::uint64_t fault_address() const noexcept { return 0; }
	[[nodiscard]] uml::memory::Access fault_access() const noexcept
	{
		return uml::memory::Access::read;
	}
	[[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept
	{
		return 0;
	}

	bool throw_on_run{true};
	bool entered{};
	std::size_t committed_before_throw{};
	std::size_t begin_calls{};
	std::size_t end_calls{};
	std::size_t block_calls{};
};

class ThrowingMemory final : public uml::cpu::GuestAddressSpace {
public:
	[[nodiscard]] bool read(
		std::uint64_t, std::span<std::byte>) override
	{
		throw std::runtime_error{"injected interpreter memory failure"};
	}

	[[nodiscard]] bool write(
		std::uint64_t, std::span<const std::byte>) override
	{
		throw std::runtime_error{"injected interpreter memory failure"};
	}
};

struct InterruptPollState final {
	std::size_t calls{};
	std::size_t pending_on_call{};
};

[[nodiscard]] bool poll_interrupt(void *opaque) noexcept
{
	auto &state = *static_cast<InterruptPollState *>(opaque);
	++state.calls;
	return state.calls >= state.pending_on_call;
}

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

class StaticGeneration final {
public:
	explicit StaticGeneration(unsigned long generation)
		: generation_{generation}
	{
		require(darwin_uml_static_vm_activate(generation_) == 0,
			"cannot activate fault-direction generation");
	}

	~StaticGeneration()
	{
		if (darwin_uml_static_vm_destroy(generation_) != 0)
			std::abort();
	}

	void map(std::uint64_t address,
		uml::emulator::tests::JitBacking &backing, int protection,
		bool guest_code = true)
	{
		require(darwin_uml_static_vm_map_generation(
			generation_, reinterpret_cast<void *>(address),
			backing.descriptor(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
			protection, guest_code ? 1 : 0,
			(protection & PROT_WRITE) != 0) == 0,
			"cannot map fault-direction page");
	}

	void protect(std::uint64_t address, int protection)
	{
		require(darwin_uml_static_vm_protect(
			reinterpret_cast<void *>(address),
			DARWIN_UML_GUEST_PAGE_SIZE, protection) == 0,
			"cannot update fault-direction protection");
	}

private:
	unsigned long generation_;
};

[[nodiscard]] std::uint64_t native_fpcr() noexcept
{
	std::uint64_t value{};
	__asm__ volatile("mrs %0, fpcr" : "=r"(value));
	return value;
}

struct AllocationBoundary final {
	std::size_t blocks{};
};

[[nodiscard]] bool fail_next_block_allocation(
	uml::cpu::arm64::CpuState &, void *opaque) noexcept
{
	auto &state = *static_cast<AllocationBoundary *>(opaque);
	if (++state.blocks == 1)
		allocation_injection::fail_next.store(true, std::memory_order_release);
	return false;
}

void test_factory_contract()
{
	for (const auto kind : {uml::cpu::EngineKind::interpreter,
		uml::cpu::EngineKind::gadgets, uml::cpu::EngineKind::jit}) {
		auto engine = uml::cpu::make_cpu_engine(kind);
		require(engine != nullptr, "engine factory returned null");
		require(engine->kind() == kind, "engine kind did not survive factory");
		if (kind != uml::cpu::EngineKind::jit)
			require(!uml::testing::process_has_writable_executable_region(),
				"non-JIT engine factory created W+X memory");
	}
	const auto invalid = static_cast<uml::cpu::EngineKind>(99);
	require(!uml::cpu::cpu_engine_available(invalid),
		"invalid engine kind was reported as available");
	require(uml::cpu::make_cpu_engine(invalid) == nullptr,
		"invalid engine kind fell back to a production engine");
}

void test_interpreter_result_mapping()
{
	auto engine = uml::cpu::make_cpu_engine(uml::cpu::EngineKind::interpreter);
	TestMemory memory;
	uml::cpu::arm64::CpuState cpu;
	cpu.pc = 0;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.generation = 1,
		.instruction_budget = 16,
	};
	const auto result = engine->run(request);
	require(result.reason == uml::cpu::ExitReason::system_call,
		"interpreter exit reason was not normalized");
	require(result.retired_instructions == 1,
		"interpreter retired count was not preserved");
	require(!uml::testing::process_has_writable_executable_region(),
		"interpreter execution created W+X memory");
}

void test_interpreter_scopes_unsupported_instruction_to_guest()
{
	/* PTRUE P0.B is architecturally allocated SVE, which is intentionally
	 * outside the current engine profile. It exercises a real implementation
	 * gap rather than an unallocated encoding.
	 */
	constexpr std::uint32_t unsupported_instruction = 0x2518'e3e0U;
	auto engine = uml::cpu::make_cpu_engine(uml::cpu::EngineKind::interpreter);
	uml::cpu::arm64::CpuState illegal_cpu;
	TestMemory illegal_memory{0x0000'0000U}; // UDF #0
	uml::cpu::ExecutionRequest illegal_request{
		.cpu = illegal_cpu,
		.memory = illegal_memory,
		.generation = 1,
		.instruction_budget = 1,
	};
	const auto illegal = engine->run(illegal_request);
	require(illegal.reason == uml::cpu::ExitReason::illegal_instruction,
		"architecturally undefined encoding was not guest-illegal");
	require(illegal.retired_instructions == 0,
		"undefined encoding unexpectedly retired");

	uml::cpu::arm64::CpuState unsupported_cpu;
	TestMemory unsupported_memory{unsupported_instruction};
	uml::cpu::ExecutionRequest unsupported_request{
		.cpu = unsupported_cpu,
		.memory = unsupported_memory,
		.generation = 1,
		.instruction_budget = 1,
	};
	const auto unsupported = engine->run(unsupported_request);
	require(unsupported.reason == uml::cpu::ExitReason::illegal_instruction,
		"valid unimplemented encoding stopped the emulator");
	require(unsupported.retired_instructions == 0,
		"unimplemented encoding unexpectedly retired");
	require(unsupported_cpu.pc == 0,
		"unimplemented encoding changed the faulting PC");

	/* Reuse the same engine object for known-good work. An implementation gap
	 * is a guest-program exit, not an engine-lifetime failure. */
	uml::cpu::arm64::CpuState recovery_cpu;
	TestMemory recovery_memory;
	uml::cpu::ExecutionRequest recovery_request{
		.cpu = recovery_cpu,
		.memory = recovery_memory,
		.generation = 2,
		.instruction_budget = 1,
	};
	const auto recovery = engine->run(recovery_request);
	require(recovery.reason == uml::cpu::ExitReason::system_call &&
		recovery.retired_instructions == 1,
		"interpreter was unusable after an unsupported instruction");
}

void test_all_engines_scope_unsupported_instruction_to_guest()
{
	using uml::emulator::tests::JitBacking;
	using uml::emulator::tests::JitMapping;
	constexpr std::uint64_t base = 0x120'0000;
	/* PTRUE P0.B: allocated SVE, intentionally outside the current engine
	 * profile. Every adapter must turn that implementation gap into a precise
	 * guest illegal-instruction exit.
	 */
	constexpr std::uint32_t unsupported_instruction = 0x2518'e3e0U;
	unsigned long generation = 0x7200;
	for (const auto kind : {uml::cpu::EngineKind::interpreter,
		uml::cpu::EngineKind::gadgets, uml::cpu::EngineKind::jit}) {
		if (!uml::cpu::cpu_engine_available(kind))
			continue;
		auto engine = uml::cpu::make_cpu_engine(kind);
		require(engine != nullptr && engine->kind() == kind,
			"requested engine was replaced by a fallback");
		{
			JitBacking backing{1};
			backing.put32(0, unsupported_instruction);
			JitMapping mapping{generation, base, backing, 1,
				PROT_READ | PROT_EXEC};
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			uml::cpu::ExecutionRequest request{
				.cpu = cpu,
				.memory = memory,
				.translated_memory = &memory,
				.generation = generation,
				.instruction_budget = 1,
			};
			const auto result = engine->run(request);
			if (result.reason !=
				    uml::cpu::ExitReason::illegal_instruction ||
			    result.retired_instructions != 0 || cpu.pc != base)
				std::fprintf(stderr,
					"unsupported diagnostics engine=%d reason=%d "
					"retired=%zu pc=%#llx\n",
					static_cast<int>(kind),
					static_cast<int>(result.reason),
					result.retired_instructions,
					static_cast<unsigned long long>(cpu.pc));
			require(result.reason ==
					uml::cpu::ExitReason::illegal_instruction &&
				result.retired_instructions == 0 &&
				cpu.pc == base,
				"engine did not preserve precise guest-illegal state");
		}
		++generation;
		{
			JitBacking backing{1};
			backing.put32(0, 0xd280'00a0U); // movz x0, #5
			backing.put32(4, unsupported_instruction);
			backing.put32(8, 0xd400'0001U); // svc #0
			JitMapping mapping{generation, base, backing, 1,
				PROT_READ | PROT_EXEC};
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			uml::cpu::ExecutionRequest request{
				.cpu = cpu,
				.memory = memory,
				.translated_memory = &memory,
				.generation = generation,
				.instruction_budget = 8,
			};
			const auto result = engine->run(request);
			require(result.reason ==
					uml::cpu::ExitReason::illegal_instruction &&
				result.retired_instructions == 1 &&
				cpu.pc == base + sizeof(std::uint32_t) &&
				cpu.registers[0] == 5,
				"engine lost a supported prefix before an implementation gap");
		}
		++generation;
		{
			JitBacking backing{1};
			backing.put32(0, 0xd400'0001U);
			JitMapping mapping{generation, base, backing, 1,
				PROT_READ | PROT_EXEC};
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			uml::cpu::ExecutionRequest request{
				.cpu = cpu,
				.memory = memory,
				.translated_memory = &memory,
				.generation = generation,
				.instruction_budget = 1,
			};
			const auto result = engine->run(request);
			require(result.reason == uml::cpu::ExitReason::system_call &&
				result.retired_instructions == 1,
				"engine was unusable after a guest implementation gap");
		}
		++generation;
	}
}

void test_all_engines_preserve_fault_direction_and_retry()
{
	using uml::emulator::tests::JitBacking;
	constexpr std::uint64_t base = 0x180'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	unsigned long generation = 0x7600;
	for (const auto kind : {uml::cpu::EngineKind::interpreter,
		uml::cpu::EngineKind::gadgets, uml::cpu::EngineKind::jit}) {
		if (!uml::cpu::cpu_engine_available(kind))
			continue;
		auto engine = uml::cpu::make_cpu_engine(kind);
		require(engine != nullptr && engine->kind() == kind,
			"fault-direction engine was replaced by a fallback");
		const auto run = [&](uml::cpu::arm64::CpuState &cpu,
			uml::memory::StaticGuestMemory &memory,
			unsigned long current_generation) {
			uml::cpu::ExecutionRequest request{
				.cpu = cpu,
				.memory = memory,
				.translated_memory = &memory,
				.generation = current_generation,
				.instruction_budget = 8,
			};
			return engine->run(request);
		};

		/* An absent instruction page is an execute fault and does not retire.
		 * Mapping it executable must make the exact same engine reusable. */
		{
			StaticGeneration state{generation};
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			const auto fault = run(cpu, memory, generation);
			require(fault.reason == uml::cpu::ExitReason::memory_fault &&
				fault.retired_instructions == 0 &&
				fault.fault.address == base &&
				fault.fault.access ==
					uml::memory::MemoryAccess::execute &&
				cpu.pc == base,
				"unmapped entry fetch lost execute-fault precision");
			JitBacking backing{1};
			backing.put32(0, 0xd400'0001U); // svc #0
			state.map(base, backing, PROT_READ | PROT_EXEC);
			const auto retry = run(cpu, memory, generation);
			require(retry.reason == uml::cpu::ExitReason::system_call &&
				retry.retired_instructions == 1,
				"execute-fault mapping retry did not resume precisely");
		}
		++generation;

		/* A mapped but non-executable entry has the same execute direction and
		 * becomes runnable after only execute permission is added. */
		{
			JitBacking backing{1};
			backing.put32(0, 0xd400'0001U); // svc #0
			StaticGeneration state{generation};
			state.map(base, backing, PROT_READ);
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			const auto fault = run(cpu, memory, generation);
			require(fault.reason == uml::cpu::ExitReason::memory_fault &&
				fault.retired_instructions == 0 &&
				fault.fault.address == base &&
				fault.fault.access ==
					uml::memory::MemoryAccess::execute &&
				cpu.pc == base,
				"non-executable entry fetch lost execute direction");
			state.protect(base, PROT_READ | PROT_EXEC);
			const auto retry = run(cpu, memory, generation);
			require(retry.reason == uml::cpu::ExitReason::system_call &&
				retry.retired_instructions == 1,
				"execute-permission retry did not resume precisely");
		}
		++generation;

		/* Execute-only code can fetch LDR, but its data read of that exact PC
		 * must remain a read fault. Address equality is not access metadata. */
		{
			JitBacking backing{1};
			backing.put32(0, 0xf940'0020U); // ldr x0, [x1]
			backing.put32(4, 0xd400'0001U); // svc #0
			StaticGeneration state{generation};
			state.map(base, backing, PROT_EXEC);
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			cpu.registers[1] = base;
			const auto fault = run(cpu, memory, generation);
			require(fault.reason == uml::cpu::ExitReason::memory_fault &&
				fault.retired_instructions == 0 &&
				fault.fault.address == base &&
				fault.fault.access == uml::memory::MemoryAccess::read &&
				cpu.pc == base,
				"data read at PC was misclassified as execute");
			state.protect(base, PROT_READ | PROT_EXEC);
			const auto retry = run(cpu, memory, generation);
			require(retry.reason == uml::cpu::ExitReason::system_call &&
				retry.retired_instructions == 2,
				"read-permission retry did not resume at the faulting load");
		}
		++generation;

		for (const auto [instruction, access] :
		     std::array{
			     std::pair{0xf940'0020U,
				     uml::memory::MemoryAccess::read},
			     std::pair{0xf900'0020U,
				     uml::memory::MemoryAccess::write}}) {
			JitBacking backing{1};
			backing.put32(0, instruction);
			backing.put32(4, 0xd400'0001U);
			StaticGeneration state{generation};
			state.map(base, backing, PROT_READ | PROT_EXEC);
			uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			uml::cpu::arm64::CpuState cpu;
			cpu.pc = base;
			cpu.registers[1] = base + page;
			const auto fault = run(cpu, memory, generation);
			require(fault.reason == uml::cpu::ExitReason::memory_fault &&
				fault.retired_instructions == 0 &&
				fault.fault.address == base + page &&
				fault.fault.access == access && cpu.pc == base,
				"ordinary data fault direction or precision changed");
			++generation;
		}
	}
}

void test_c_bridge_preserves_fault_direction_and_retry()
{
	using EntryPoint = int (*)(void *, void *, unsigned long,
		unsigned long, unsigned long *);
	using uml::emulator::tests::JitBacking;
	struct Engine final {
		uml::cpu::EngineKind kind;
		EntryPoint enter;
	};
	constexpr std::array engines{
		Engine{uml::cpu::EngineKind::interpreter,
			&darwin_uml_interpreter_enter_guest},
		Engine{uml::cpu::EngineKind::gadgets,
			&darwin_uml_static_enter_guest},
		Engine{uml::cpu::EngineKind::jit,
			&darwin_uml_jit_enter_guest},
	};
	constexpr std::uint64_t base = 0x1a0'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	unsigned long generation = 0x7700;

	for (const auto &engine : engines) {
		if (!uml::cpu::cpu_engine_available(engine.kind))
			continue;

		/* Start with readable, non-executable code. After the execute fault,
		 * make it execute-only: the LDR's data address intentionally equals
		 * its PC, proving that the bridge exports direction rather than an
		 * address-based guess. */
		{
			JitBacking code{1};
			code.put32(0, 0xf940'0020U); // ldr x0, [x1]
			code.put32(4, 0xd400'0001U); // svc #0
			StaticGeneration state{generation};
			state.map(base, code, PROT_READ);
			darwin_uml_guest_registers registers{};
			darwin_uml_guest_fp_registers fp{};
			registers.pc = base;
			registers.x[1] = base;
			unsigned long tls = 0;
			const auto execute_fault = engine.enter(&registers, &fp,
				sizeof(fp), generation, &tls);
			require(execute_fault == DARWIN_UML_EVENT_FAULT &&
				registers.fault.address == base && registers.pc == base &&
				registers.fault.error_code == DARWIN_UML_FAULT_EXECUTE,
				"C bridge lost non-executable fetch direction");

			state.protect(base, PROT_EXEC);
			const auto read_fault = engine.enter(&registers, &fp,
				sizeof(fp), generation, &tls);
			require(read_fault == DARWIN_UML_EVENT_FAULT &&
				registers.fault.address == base && registers.pc == base &&
				registers.fault.error_code == 0,
				"C bridge misclassified a data read at PC as execute");

			state.protect(base, PROT_READ | PROT_EXEC);
			const auto retry = engine.enter(&registers, &fp,
				sizeof(fp), generation, &tls);
			require(retry == DARWIN_UML_EVENT_SYSCALL &&
				registers.pc == base + 8,
				"C bridge read-fault retry was not precise");
		}
		++generation;

		{
			JitBacking code{1};
			JitBacking data{1};
			code.put32(0, 0xf900'0020U); // str x0, [x1]
			code.put32(4, 0xd400'0001U); // svc #0
			StaticGeneration state{generation};
			state.map(base, code, PROT_READ | PROT_EXEC);
			state.map(base + page, data, PROT_READ, false);
			darwin_uml_guest_registers registers{};
			darwin_uml_guest_fp_registers fp{};
			registers.pc = base;
			registers.x[0] = 0x1122'3344'5566'7788ULL;
			registers.x[1] = base + page;
			unsigned long tls = 0;
			const auto write_fault = engine.enter(&registers, &fp,
				sizeof(fp), generation, &tls);
			require(write_fault == DARWIN_UML_EVENT_FAULT &&
				registers.fault.address == base + page &&
				registers.fault.error_code == DARWIN_UML_FAULT_WRITE &&
				registers.pc == base,
				"C bridge lost write-fault direction or precision");

			state.protect(base + page, PROT_READ | PROT_WRITE);
			const auto retry = engine.enter(&registers, &fp,
				sizeof(fp), generation, &tls);
			require(retry == DARWIN_UML_EVENT_SYSCALL &&
				registers.pc == base + 8,
				"C bridge write-fault retry was not precise");
		}
		++generation;
	}
}

void test_production_translated_exception_contract()
{
	using uml::emulator::tests::JitBacking;
	constexpr std::uint64_t base = 0x1c0'0000;
	constexpr std::uint64_t target = base + 0x10'0000;
	const auto branch_delta = target - (base + sizeof(std::uint32_t));
	const auto branch = 0x1400'0000U |
		(static_cast<std::uint32_t>(branch_delta >> 2U) & 0x03ff'ffffU);
	unsigned long generation = 0x7800;

	for (const auto kind : {uml::cpu::EngineKind::gadgets,
		uml::cpu::EngineKind::jit}) {
		if (!uml::cpu::cpu_engine_available(kind))
			continue;
		auto engine = uml::cpu::make_cpu_engine(kind);
		JitBacking first{1};
		JitBacking second{1};
		first.put32(0, 0x1e22'2820U); // fadd s0, s1, s2
		first.put32(4, branch);        // b target
		second.put32(0, 0xd400'0001U); // svc #0
		StaticGeneration state{generation};
		state.map(base, first, PROT_READ | PROT_EXEC);
		state.map(target, second, PROT_READ | PROT_EXEC);
		uml::memory::StaticGuestMemory memory;
		memory.set_generation(generation);
		uml::cpu::arm64::CpuState cpu;
		cpu.pc = base;
		const std::uint32_t one = 0x3f80'0000U;
		std::memcpy(cpu.vectors[1].bytes.data(), &one, sizeof(one));
		std::memcpy(cpu.vectors[2].bytes.data(), &one, sizeof(one));
		const auto caller_fpcr = native_fpcr();
		AllocationBoundary boundary;
		uml::cpu::ExecutionRequest request{
			.cpu = cpu,
			.memory = memory,
			.translated_memory = &memory,
			.generation = generation,
			.instruction_budget = 16,
			.boundary = fail_next_block_allocation,
			.boundary_context = &boundary,
		};
		const auto failed = engine->run(request);
		require(failed.reason == uml::cpu::ExitReason::fatal &&
			failed.retired_instructions == 2 && cpu.pc == target,
			"second-block allocation failure lost its retired prefix");
		require(boundary.blocks == 1 &&
			!allocation_injection::fail_next.load(std::memory_order_acquire),
			"second-block allocation injection was not consumed precisely");
		require(!cpu.gadget_fp_active && !cpu.gadget_fpmr_active &&
			native_fpcr() == caller_fpcr,
			"fatal translated cleanup leaked the guest host environment");

		request.boundary = nullptr;
		request.boundary_context = nullptr;
		const auto retry = engine->run(request);
		require(retry.reason == uml::cpu::ExitReason::system_call &&
			retry.retired_instructions == 1 && cpu.pc == target + 4,
			"translated engine was not reusable after exception cleanup");
		require(!cpu.gadget_fp_active && native_fpcr() == caller_fpcr,
			"successful retry did not retain balanced host state");
		++generation;
	}
}

void test_gadget_post_execution_failure_preserves_prefix_and_reuse()
{
	using uml::emulator::tests::JitBacking;
	if (!uml::cpu::cpu_engine_available(uml::cpu::EngineKind::gadgets))
		return;

	constexpr std::uint64_t base = 0x1e0'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint64_t first_target = base + page;
	constexpr std::uint64_t second_target = base + 2 * page;
	constexpr unsigned long generation = 0x7900;
	auto engine = uml::cpu::make_cpu_engine(uml::cpu::EngineKind::gadgets);
	require(engine != nullptr &&
		engine->kind() == uml::cpu::EngineKind::gadgets,
		"gadget post-execution test did not receive the requested engine");

	JitBacking source{1};
	JitBacking first{1};
	JitBacking second{1};
	source.put32(0, 0xd61f'0000U); // indirect transfer through x0
	first.put32(0, 0xd400'0001U);  // svc #0
	second.put32(0, 0xd400'0001U); // svc #0
	StaticGeneration state{generation};
	state.map(base, source, PROT_READ | PROT_EXEC);
	state.map(first_target, first, PROT_READ | PROT_EXEC);
	state.map(second_target, second, PROT_READ | PROT_EXEC);
	uml::memory::StaticGuestMemory memory;
	memory.set_generation(generation);
	uml::cpu::arm64::CpuState cpu;
	cpu.pc = base;
	cpu.registers[0] = first_target;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = generation,
		.instruction_budget = 8,
	};
	const auto caller_fpcr = native_fpcr();
	const auto warm = engine->run(request);
	require(warm.reason == uml::cpu::ExitReason::system_call &&
		warm.retired_instructions == 2 && cpu.pc == first_target + 4,
		"gadget indirect-link cache warmup did not reach its boundary");
	require(cpu.gadget_indirect_miss == nullptr &&
		!cpu.gadget_fp_active && !cpu.gadget_fpmr_active &&
		native_fpcr() == caller_fpcr,
		"gadget cache warmup leaked transient execution state");

	/* The source block and its first target are now fully cached. Force the
	 * allocation that learns a different target to fail after the transfer has
	 * already retired and advanced the architectural PC. */
	cpu.pc = base;
	cpu.registers[0] = second_target;
	allocation_injection::fail_next.store(true, std::memory_order_release);
	const auto failed = engine->run(request);
	require(failed.reason == uml::cpu::ExitReason::fatal &&
		failed.retired_instructions == 1 && cpu.pc == second_target,
		"post-execution cache failure lost the committed instruction prefix");
	require(!allocation_injection::fail_next.load(std::memory_order_acquire),
		"post-execution cache allocation injection was not consumed");
	require(cpu.gadget_indirect_miss == nullptr &&
		!cpu.gadget_fp_active && !cpu.gadget_fpmr_active &&
		native_fpcr() == caller_fpcr,
		"post-execution cache failure leaked executor-private state");

	const auto retry = engine->run(request);
	require(retry.reason == uml::cpu::ExitReason::system_call &&
		retry.retired_instructions == 1 && cpu.pc == second_target + 4,
		"gadget engine was not reusable after post-execution failure");
	require(cpu.gadget_indirect_miss == nullptr &&
		!cpu.gadget_fp_active && native_fpcr() == caller_fpcr,
		"gadget retry did not retain balanced execution state");
}

void test_interpreter_contains_memory_exceptions()
{
	auto engine = uml::cpu::make_cpu_engine(uml::cpu::EngineKind::interpreter);
	ThrowingMemory memory;
	uml::cpu::arm64::CpuState cpu;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.generation = 1,
		.instruction_budget = 1,
	};
	const auto result = engine->run(request);
	require(result.reason == uml::cpu::ExitReason::fatal,
		"interpreter exception escaped the noexcept CPU boundary");
	require(result.retired_instructions == 0,
		"failed interpreter memory access unexpectedly retired");
}

void test_translated_internal_interrupt_obeys_fairness_budget()
{
	TranslatedBoundaryExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = 7,
		.instruction_budget = 64 * 1024,
	};
	const auto result = uml::cpu::arm64::detail::run_translated(
		executor, request);
	require(result.reason == uml::cpu::ExitReason::instruction_budget,
		"internal translated event escaped as a guest interrupt");
	require(result.retired_instructions == 64 * 1024,
		"translated fairness retirement count changed");
	require(executor.block_calls == 16,
		"translated engine did not run through the exact fairness budget");
	require(executor.begin_calls == 1 && executor.end_calls == 1,
		"translated execution lifetime was not balanced");
}

void test_translated_internal_interrupt_delivers_pending_interrupt()
{
	TranslatedBoundaryExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu;
	InterruptPollState poll{.pending_on_call = 2};
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = 11,
		.instruction_budget = 64 * 1024,
		.interrupt = {
			.poll = poll_interrupt,
			.poll_context = &poll,
		},
	};
	const auto result = uml::cpu::arm64::detail::run_translated(
		executor, request);
	require(result.reason == uml::cpu::ExitReason::interrupt,
		"pending host/guest interrupt was not delivered");
	require(result.retired_instructions == TranslatedBoundaryExecutor::block_size,
		"interrupt boundary lost the exact retired count");
	require(executor.block_calls == 1 && poll.calls == 2,
		"interrupt was not sampled at the translated cold boundary");
	require(executor.begin_calls == 1 && executor.end_calls == 1,
		"interrupt exit did not close translated execution");
}

void test_translated_cleanup_is_exception_safe()
{
	ThrowingTranslatedExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = 13,
		.instruction_budget = 1,
	};
	const auto failed =
		uml::cpu::arm64::detail::run_translated(executor, request);
	require(failed.reason == uml::cpu::ExitReason::fatal &&
		failed.retired_instructions == 0,
		"translated failure was not contained at the engine boundary");
	require(executor.begin_calls == 1 && executor.end_calls == 1,
		"throwing translated entry did not perform exactly one cleanup");
	require(!executor.entered,
		"throwing translated entry left its execution context entered");

	executor.committed_before_throw = 3;
	const auto postamble_failed =
		uml::cpu::arm64::detail::run_translated(executor, request);
	require(postamble_failed.reason == uml::cpu::ExitReason::fatal &&
		postamble_failed.retired_instructions == 3,
		"translated postamble failure lost its committed prefix");
	require(executor.begin_calls == 2 && executor.end_calls == 2 &&
		!executor.entered,
		"translated postamble failure did not retain balanced cleanup");

	executor.throw_on_run = false;
	executor.committed_before_throw = 0;
	const auto retry = uml::cpu::arm64::detail::run_translated(executor, request);
	require(retry.reason == uml::cpu::ExitReason::fatal,
		"translated entry could not be retried after exception cleanup");
	require(executor.begin_calls == 3 && executor.end_calls == 3,
		"retried translated entry did not retain balanced cleanup");
}

} // namespace

int main()
{
	try {
		test_factory_contract();
		test_interpreter_result_mapping();
		test_interpreter_scopes_unsupported_instruction_to_guest();
		test_all_engines_scope_unsupported_instruction_to_guest();
		test_all_engines_preserve_fault_direction_and_retry();
		test_c_bridge_preserves_fault_direction_and_retry();
		test_production_translated_exception_contract();
		test_gadget_post_execution_failure_preserves_prefix_and_reuse();
		test_interpreter_contains_memory_exceptions();
		test_translated_internal_interrupt_obeys_fairness_budget();
		test_translated_internal_interrupt_delivers_pending_interrupt();
		test_translated_cleanup_is_exception_safe();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "CPU engine contract test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
}
