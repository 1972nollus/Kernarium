/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The guest loop in which the x86 SIMD floating-point tests run one
 * register-form instruction over rows of operands: one flat mapping holds
 * code, stack, inputs and outputs, the loop loads ymm0 to ymm2 and MXCSR,
 * runs the instruction, and stores ymm0, RAX, RFLAGS and MXCSR for each row,
 * through the gadget engine or, with UML_X86_64_SSE_LOOP_JIT, the JIT. */
#pragma once

#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/exception_checks.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#if UML_X86_64_SSE_LOOP_JIT
#include "cpu/x86_64/jit/fex_engine.hpp"
#include "platform/apple/host_features.hpp"
#endif
#include "memory/guest_address_space.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace uml::test::x86_64_sse_loop {

using uml::cpu::x86_64::CpuState;
using uml::cpu::x86_64::ExecutionMode;
using uml::cpu::x86_64::Register;

/* One flat mapping from address zero: code, stack, inputs and outputs. */
class FlatMemory final : public uml::memory::GuestAddressSpace {
public:
	explicit FlatMemory(std::size_t size) : bytes_(size) {}

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		if (!inside(address, destination.size()))
			return fault(address, uml::memory::MemoryAccess::read);
		std::memcpy(destination.data(), bytes_.data() + address,
			destination.size());
		return true;
	}

	[[nodiscard]] bool fetch(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		if (!inside(address, destination.size()))
			return fault(address, uml::memory::MemoryAccess::execute);
		std::memcpy(destination.data(), bytes_.data() + address,
			destination.size());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> source) override
	{
		if (!inside(address, source.size()))
			return fault(address, uml::memory::MemoryAccess::write);
		std::memcpy(bytes_.data() + address, source.data(), source.size());
		return true;
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, uml::memory::MemoryAccess access) override
	{
		if (!inside(address, size)) {
			(void)fault(address, access);
			return nullptr;
		}
		return bytes_.data() + address;
	}

	[[nodiscard]] uml::memory::MemoryFault last_fault() const noexcept override
	{
		return fault_;
	}

	[[nodiscard]] bool has_fault() const noexcept override { return faulted_; }

#if UML_X86_64_SSE_LOOP_JIT
	/* The JIT fetches only from executable pages. */
	[[nodiscard]] bool is_executable(std::uint64_t address) noexcept override
	{
		return address < bytes_.size();
	}
#endif

	[[nodiscard]] std::byte *at(std::uint64_t address) noexcept
	{
		return bytes_.data() + address;
	}

private:
	[[nodiscard]] bool inside(std::uint64_t address,
		std::size_t size) const noexcept
	{
		return address <= bytes_.size() && size <= bytes_.size() - address;
	}

	[[nodiscard]] bool fault(std::uint64_t address,
		uml::memory::MemoryAccess access) noexcept
	{
		fault_ = {address, access};
		faulted_ = true;
		return false;
	}

	std::vector<std::byte> bytes_;
	uml::memory::MemoryFault fault_{};
	bool faulted_{};
};

/* ---- the guest loop ---- */

constexpr std::uint64_t code_address = 0x1000;
constexpr std::uint64_t stack_top = 0x8000;
constexpr std::uint64_t input_address = 0x10000;
constexpr std::size_t input_stride = 96;
constexpr std::size_t output_stride = 56;
/* MXCSR for the cell, which the loop loads before each instruction. */
constexpr std::uint64_t control_address = 0x9000;
/* The instruction's offset in the loop. */
constexpr std::uint64_t instruction_offset = 17;
constexpr std::size_t chunk = 16384;
constexpr std::uint64_t output_address = input_address + chunk * input_stride;
constexpr std::size_t memory_size = output_address + chunk * output_stride;

/* Loads ymm0, ymm1 and ymm2, runs the instruction, and stores ymm0, RAX
 * and RFLAGS, `rcx` times. */
[[nodiscard]] inline std::vector<std::byte> loop_code(
	std::span<const std::uint8_t> instruction)
{
	std::vector<std::uint8_t> code{
		0xc5, 0xfe, 0x6f, 0x06,             /* vmovdqu ymm0, [rsi] */
		0xc5, 0xfe, 0x6f, 0x4e, 0x20,       /* vmovdqu ymm1, [rsi+32] */
		0xc5, 0xfe, 0x6f, 0x56, 0x40,       /* vmovdqu ymm2, [rsi+64] */
		0x0f, 0xae, 0x13,                   /* ldmxcsr [rbx] */
	};
	code.insert(code.end(), instruction.begin(), instruction.end());
	code.insert(code.end(), {
		0x9c, 0x5a,                         /* pushfq; pop rdx */
		0xc5, 0xfe, 0x7f, 0x07,             /* vmovdqu [rdi], ymm0 */
		0x48, 0x89, 0x47, 0x20,             /* mov [rdi+32], rax */
		0x48, 0x89, 0x57, 0x28,             /* mov [rdi+40], rdx */
		0x0f, 0xae, 0x5f, 0x30,             /* stmxcsr [rdi+48] */
		0x48, 0x83, 0xc6, 0x60,             /* add rsi, 96 */
		0x48, 0x83, 0xc7, 0x38,             /* add rdi, 56 */
		0xff, 0xc9,                         /* dec ecx */
	});
	const auto back = -static_cast<int>(code.size() + 2U);
	code.push_back(0x75);                   /* jnz loop */
	code.push_back(static_cast<std::uint8_t>(back));
	code.insert(code.end(), {0x0f, 0x05}); /* syscall */
	std::vector<std::byte> bytes(code.size());
	std::memcpy(bytes.data(), code.data(), code.size());
	return bytes;
}

#if UML_X86_64_SSE_LOOP_JIT
/* The x86-64 JIT, which serves the process as it serves a vCPU thread. Each
 * run starts a fresh address-space generation, so no translation crosses
 * cells. */
struct JitEngine final {
	uml::cpu::x86_64::jit::FexEngine engine{
		uml::platform::apple::probed_host_capabilities()};
	unsigned long generation{};
	uml::cpu::InterruptToken interrupt{};
};

inline JitEngine &jit_engine()
{
	static JitEngine engine;
	return engine;
}

/* The same through the JIT. An instruction that faults with #XM is marked
 * in `trapped` and skipped, as for the gadget engine. */
[[nodiscard]] inline bool run_engine(FlatMemory &memory, JitEngine &jit,
	std::uint32_t mxcsr, std::size_t count, bool checked = false,
	std::size_t instruction_length = 0,
	std::vector<std::uint8_t> *trapped = nullptr)
{
	using namespace uml::cpu::x86_64;
	CpuState cpu{};
	cpu.mxcsr = mxcsr;
	cpu.rip = code_address;
	cpu.rflags = 0x202;
	cpu.gpr(Register::rsp) = stack_top;
	cpu.gpr(Register::rsi) = input_address;
	cpu.gpr(Register::rdi) = output_address;
	cpu.gpr(Register::rcx) = count;
	cpu.gpr(Register::rbx) = control_address;
	const auto generation = ++jit.generation;
	jit.engine.apply_changes(generation, ExecutionMode::long64, {
		.mapping_sequence = 1,
		.code_sequence = 1,
		.all_mappings = true,
		.all_code = true,
	});
	for (std::size_t dispatch = 0; dispatch < count * 16U + 64U;
		++dispatch) {
		const auto result = jit.engine.run(cpu, memory, {
			.generation = generation,
			.instruction_budget = 1U << 16U,
			.execution_context = nullptr,
			.interrupt = &jit.interrupt,
		}).execution;
		if (result.reason == uml::cpu::ExitReason::system_call)
			return cpu.gpr(Register::rsi) ==
				input_address + count * input_stride;
		if (result.reason == uml::cpu::ExitReason::instruction_budget) {
			cpu.rflags &= ~(1ULL << 16U);
			continue;
		}
		if (checked && trapped != nullptr &&
			result.reason == uml::cpu::ExitReason::floating_point_trap &&
			cpu.rip == code_address + instruction_offset) {
			const auto row = (cpu.gpr(Register::rsi) - input_address) /
				input_stride;
			if (row >= trapped->size())
				return false;
			(*trapped)[row] = 1;
			cpu.rip += instruction_length;
			continue;
		}
		return false;
	}
	return false;
}
#endif

/* Runs the loop over `count` rows. A checked run's executor builds for
 * #XM, and an instruction that faults with it is marked in `trapped` and
 * skipped, so that the loop stores the destination it kept. */
[[nodiscard]] inline bool run_engine(FlatMemory &memory,
	uml::cpu::x86_64::gadgets::Executor &executor, std::uint32_t mxcsr,
	std::size_t count, bool checked = false,
	std::size_t instruction_length = 0,
	std::vector<std::uint8_t> *trapped = nullptr)
{
	using namespace uml::cpu::x86_64;
	CpuState cpu{};
	cpu.mxcsr = mxcsr;
	cpu.rip = code_address;
	cpu.rflags = 0x202;
	cpu.gpr(Register::rsp) = stack_top;
	cpu.gpr(Register::rsi) = input_address;
	cpu.gpr(Register::rdi) = output_address;
	cpu.gpr(Register::rcx) = count;
	cpu.gpr(Register::rbx) = control_address;
	for (std::size_t dispatch = 0; dispatch < count * 16U + 64U;
		++dispatch) {
		const auto selected = executor.select_program(memory, cpu.rip,
			ExecutionMode::long64);
		if (selected.status != gadgets::BuildStatus::built ||
			selected.program == nullptr)
			return false;
		const auto result = checked ?
			gadgets::execute_checked(*selected.program, cpu, memory,
				executor, 1U << 20U, {}, false,
				gadgets::checked_exceptions(cpu)) :
			gadgets::execute(*selected.program, cpu, memory, executor,
				1U << 20U);
		/* SYSCALL leaves its return address in RCX. */
		if (result.event == uml::cpu::EngineEvent::system_call)
			return cpu.gpr(Register::rsi) ==
				input_address + count * input_stride;
		if (checked && trapped != nullptr &&
			result.event == uml::cpu::EngineEvent::floating_point_trap &&
			result.vector == 19 && result.error == cpu.mxcsr &&
			cpu.rip == code_address + instruction_offset) {
			const auto row = (cpu.gpr(Register::rsi) - input_address) /
				input_stride;
			if (row >= trapped->size())
				return false;
			(*trapped)[row] = 1;
			cpu.rip += instruction_length;
			continue;
		}
		if (result.event != uml::cpu::EngineEvent::continued)
			return false;
	}
	return false;
}

} // namespace uml::test::x86_64_sse_loop
