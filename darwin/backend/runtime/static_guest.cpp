// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/static/gadget_executor.hpp"
#include "runtime/static/guest_memory.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace uml::backend {
namespace {

constexpr std::size_t dispatch_budget = 64 * 1024;

enum class DispatchExit : std::size_t {
	system_call,
	memory_fault,
	interrupt,
	budget,
	illegal_instruction,
	fatal,
	count,
};

[[nodiscard]] bool dispatch_profile_enabled() noexcept
{
	static const bool enabled = [] {
		const auto *setting = std::getenv("UML_PROFILE_DISPATCH");
		return setting != nullptr && setting[0] != '\0' && setting[0] != '0';
	}();
	return enabled;
}

using DispatchClock = std::chrono::steady_clock;

void profile_dispatch(DispatchClock::time_point started,
		      std::size_t instructions, DispatchExit reason)
{
	if (!dispatch_profile_enabled())
		return;
	struct Statistics final {
		std::uint64_t calls{};
		std::uint64_t nanoseconds{};
		std::uint64_t instructions{};
		std::uint64_t maximum_nanoseconds{};
		std::array<std::uint64_t,
			static_cast<std::size_t>(DispatchExit::count)> exits{};
	};
	static thread_local Statistics statistics;
	const auto elapsed = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			DispatchClock::now() - started).count());
	++statistics.calls;
	statistics.nanoseconds += elapsed;
	statistics.instructions += instructions;
	statistics.maximum_nanoseconds = std::max(
		statistics.maximum_nanoseconds, elapsed);
	++statistics.exits[static_cast<std::size_t>(reason)];
	if ((statistics.calls & 255U) != 0)
		return;
	std::fprintf(stderr,
		"Darwin dispatch profile calls=%llu average-us=%.1f max-us=%.1f "
		"average-instructions=%.1f syscall=%llu fault=%llu interrupt=%llu "
		"budget=%llu illegal=%llu fatal=%llu\n",
		static_cast<unsigned long long>(statistics.calls),
		statistics.nanoseconds / (statistics.calls * 1000.0),
		statistics.maximum_nanoseconds / 1000.0,
		statistics.instructions / static_cast<double>(statistics.calls),
		static_cast<unsigned long long>(statistics.exits[0]),
		static_cast<unsigned long long>(statistics.exits[1]),
		static_cast<unsigned long long>(statistics.exits[2]),
		static_cast<unsigned long long>(statistics.exits[3]),
		static_cast<unsigned long long>(statistics.exits[4]),
		static_cast<unsigned long long>(statistics.exits[5]));
}

struct GuestInterruptSequence final {
	const unsigned int *sequence{};
	unsigned int snapshot{};
};

GuestInterruptSequence &guest_interrupt_sequence() noexcept
{
	static thread_local GuestInterruptSequence interrupt;
	return interrupt;
}

[[nodiscard]] bool guest_interrupt_pending() noexcept
{
	const auto &interrupt = guest_interrupt_sequence();
	return interrupt.sequence != nullptr &&
		__atomic_load_n(interrupt.sequence, __ATOMIC_ACQUIRE) !=
			interrupt.snapshot;
}

void load_cpu(arm64::CpuState &cpu,
	      const darwin_uml_guest_registers &registers,
	      const darwin_uml_guest_fp_registers &fp, std::uint64_t tls)
{
	std::copy(std::begin(registers.x), std::end(registers.x),
		  cpu.registers.begin());
	cpu.registers[29] = registers.fp;
	cpu.registers[30] = registers.lr;
	cpu.sp = registers.sp;
	cpu.pc = registers.pc;
	cpu.nzcv = registers.pstate;
	cpu.tls = tls;
	for (std::size_t index = 0; index < cpu.vectors.size(); ++index)
		std::memcpy(cpu.vectors[index].bytes.data(), fp.vectors[index], 16);
	cpu.fpsr = fp.fpsr;
	cpu.fpcr = fp.fpcr;
}

void save_cpu(const arm64::CpuState &cpu,
	      darwin_uml_guest_registers &registers,
	      darwin_uml_guest_fp_registers &fp, unsigned long &tls)
{
	std::copy_n(cpu.registers.begin(), 29, std::begin(registers.x));
	registers.fp = cpu.registers[29];
	registers.lr = cpu.registers[30];
	registers.sp = cpu.sp;
	registers.pc = cpu.pc;
	registers.pstate = cpu.nzcv;
	tls = static_cast<unsigned long>(cpu.tls);
	for (std::size_t index = 0; index < cpu.vectors.size(); ++index)
		std::memcpy(fp.vectors[index], cpu.vectors[index].bytes.data(), 16);
	fp.fpsr = cpu.fpsr;
	fp.fpcr = cpu.fpcr;
}

[[nodiscard]] bool prepare_signal_return(arm64::CpuState &cpu) noexcept
{
	if (cpu.pc != DARWIN_UML_SIGRETURN_TRAMPOLINE)
		return false;
	cpu.registers[8] = DARWIN_UML_NR_RT_SIGRETURN;
	cpu.pc += sizeof(std::uint32_t);
	return true;
}

class GuestExecutionScope final {
public:
	GuestExecutionScope(GadgetExecutor &executor, unsigned long generation,
			    std::uint64_t guest_pc)
		: executor_{executor}, entered_{
			executor.begin_execution(generation, guest_pc)}
	{
	}

	GuestExecutionScope(const GuestExecutionScope &) = delete;
	GuestExecutionScope &operator=(const GuestExecutionScope &) = delete;

	~GuestExecutionScope()
	{
		if (entered_)
			executor_.end_execution();
	}

	[[nodiscard]] bool entered() const noexcept { return entered_; }

private:
	GadgetExecutor &executor_;
	bool entered_;
};

} // namespace
} // namespace uml::backend

extern "C" void darwin_uml_static_guest_interrupt_sequence(
	const unsigned int *sequence, unsigned int snapshot)
{
	auto &interrupt = uml::backend::guest_interrupt_sequence();
	interrupt.sequence = sequence;
	interrupt.snapshot = snapshot;
}

extern "C" int darwin_uml_static_enter_guest(
	void *register_pointer, void *fp_pointer, unsigned long fp_size,
	unsigned long generation, unsigned long *tls)
{
	if (register_pointer == nullptr || fp_pointer == nullptr || tls == nullptr ||
	    fp_size < sizeof(darwin_uml_guest_fp_registers))
		return DARWIN_UML_EVENT_FATAL;
	auto &registers = *static_cast<darwin_uml_guest_registers *>(register_pointer);
	auto &fp = *static_cast<darwin_uml_guest_fp_registers *>(fp_pointer);

	/* The signal restorer is a fixed UML ABI sentinel. It deliberately has no
	 * guest mapping and must be recognized before instruction fetch.
	 */
	if (registers.pc == DARWIN_UML_SIGRETURN_TRAMPOLINE) {
		registers.x[8] = DARWIN_UML_NR_RT_SIGRETURN;
		registers.pc += sizeof(std::uint32_t);
		return DARWIN_UML_EVENT_SYSCALL;
	}

	uml::backend::arm64::CpuState cpu{};
	uml::backend::load_cpu(cpu, registers, fp, *tls);
	static thread_local uml::backend::StaticGuestMemory memory;
	static thread_local uml::backend::GadgetExecutor executor;
	uml::backend::GuestExecutionScope execution{
		executor, generation, cpu.pc};
	if (!execution.entered())
		return DARWIN_UML_EVENT_FATAL;
	const auto dispatch_started = uml::backend::DispatchClock::now();
	std::size_t gadget_instructions = 0;
	for (;;) {
		/* Linux IPIs update a per-vCPU sequence before waking the Darwin
		 * thread.  Observe it at translated-block granularity so a busy guest
		 * cannot retain its vCPU until the much larger fairness budget ends.
		 */
		if (uml::backend::guest_interrupt_pending()) {
			uml::backend::save_cpu(cpu, registers, fp, *tls);
			uml::backend::profile_dispatch(dispatch_started,
				gadget_instructions,
				uml::backend::DispatchExit::interrupt);
			return DARWIN_UML_EVENT_INTERRUPT;
		}
		std::size_t block_instructions = 0;
		const auto event = executor.run_block(
			cpu, memory, generation, block_instructions);
		gadget_instructions += block_instructions;
		if (executor.preempted()) {
			uml::backend::save_cpu(cpu, registers, fp, *tls);
			uml::backend::profile_dispatch(dispatch_started,
				gadget_instructions,
				uml::backend::DispatchExit::interrupt);
			return DARWIN_UML_EVENT_INTERRUPT;
		}
		if (uml::backend::prepare_signal_return(cpu)) {
			uml::backend::save_cpu(cpu, registers, fp, *tls);
			uml::backend::profile_dispatch(dispatch_started,
				gadget_instructions,
				uml::backend::DispatchExit::system_call);
			return DARWIN_UML_EVENT_SYSCALL;
		}
		switch (event) {
		case uml::backend::arm64::gadgets::Event::system_call:
			uml::backend::save_cpu(cpu, registers, fp, *tls);
			uml::backend::profile_dispatch(dispatch_started,
				gadget_instructions,
				uml::backend::DispatchExit::system_call);
			return DARWIN_UML_EVENT_SYSCALL;
		case uml::backend::arm64::gadgets::Event::memory_fault:
			uml::backend::save_cpu(cpu, registers, fp, *tls);
			registers.fault.error_code =
				executor.fault_access() ==
					uml::backend::arm64::gadgets::Access::write ?
					DARWIN_UML_FAULT_WRITE : 0;
			registers.fault.address = executor.fault_address();
			registers.fault.trap_number = DARWIN_UML_FAULT_PAGE;
			uml::backend::profile_dispatch(dispatch_started,
				gadget_instructions,
				uml::backend::DispatchExit::memory_fault);
			return DARWIN_UML_EVENT_FAULT;
		case uml::backend::arm64::gadgets::Event::interrupt:
		case uml::backend::arm64::gadgets::Event::continued:
			if (gadget_instructions < uml::backend::dispatch_budget)
				continue;
			uml::backend::save_cpu(cpu, registers, fp, *tls);
			uml::backend::profile_dispatch(dispatch_started,
				gadget_instructions,
				uml::backend::DispatchExit::budget);
			return DARWIN_UML_EVENT_INTERRUPT;
		case uml::backend::arm64::gadgets::Event::illegal_instruction:
			uml::backend::save_cpu(cpu, registers, fp, *tls);
			uml::backend::profile_dispatch(dispatch_started,
				gadget_instructions,
				uml::backend::DispatchExit::illegal_instruction);
			return DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION;
		case uml::backend::arm64::gadgets::Event::fatal:
			uml::backend::save_cpu(cpu, registers, fp, *tls);
			uml::backend::profile_dispatch(dispatch_started,
				gadget_instructions,
				uml::backend::DispatchExit::fatal);
			return DARWIN_UML_EVENT_FATAL;
		}
	}
}
