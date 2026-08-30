// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory/static_guest_memory.hpp"
#include "cpu/engine_factory.hpp"
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
#include "cpu/x86_64/gadgets/program.hpp"
#endif
#include "platform/apple/host_features.hpp"
#include "cpu/engine.hpp"
#include "cpu/floating_point_contract.hpp"
#include "runtime/context.hpp"
#include "runtime/execution_state.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_cpu.h"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef UML_RUNTIME_PROFILING
#define UML_RUNTIME_PROFILING 0
#endif

#if UML_RUNTIME_PROFILING
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <time.h>
#endif

namespace uml::emulator {

namespace arm64 = ::uml::cpu::arm64;
namespace x86_64 = ::uml::cpu::x86_64;
using ::uml::memory::StaticGuestMemory;
namespace {

#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
constexpr std::int32_t x86_64_guest_isa = 2;
constexpr std::uint64_t x86_64_rt_sigreturn = 15;
constexpr std::uint32_t x86_64_fp_marker = 0x5838'3634U;
constexpr std::size_t x86_64_register_count = 16;
constexpr std::size_t x86_64_rflags_slot = 16;
constexpr std::size_t x86_64_fs_base_slot = 17;
constexpr std::size_t x86_64_gs_base_slot = 18;
#endif

constexpr std::size_t dispatch_budget = 64 * 1024;
constexpr std::uint32_t pstate_nzcv = 0xf000'0000U;
constexpr std::uint32_t pstate_tco = 0x0200'0000U;
constexpr std::uint32_t pstate_dit = 0x0100'0000U;
constexpr std::uint32_t pstate_ssbs = 0x0000'1000U;
constexpr std::uint32_t pstate_daif = 0x0000'03c0U;
constexpr std::uint64_t svcr_sm = 1U;
constexpr std::uint64_t svcr_za = 2U;
constexpr std::uint32_t public_floating_point_flags =
	DARWIN_UML_FLOATING_POINT_INVALID |
	DARWIN_UML_FLOATING_POINT_DIVIDE_BY_ZERO |
	DARWIN_UML_FLOATING_POINT_OVERFLOW |
	DARWIN_UML_FLOATING_POINT_UNDERFLOW |
	DARWIN_UML_FLOATING_POINT_INEXACT |
	DARWIN_UML_FLOATING_POINT_INPUT_DENORMAL;
static_assert(static_cast<int>(
		      ::uml::cpu::EngineEvent::floating_point_trap) ==
	      DARWIN_UML_EVENT_FLOATING_POINT_TRAP);
static_assert(static_cast<int>(::uml::cpu::EngineEvent::pc_alignment) ==
	      DARWIN_UML_EVENT_PC_ALIGNMENT);
static_assert(::uml::cpu::floating_point_status_exception_mask ==
	      public_floating_point_flags);
static_assert(::uml::cpu::floating_point_syndrome(0) ==
	      DARWIN_UML_FLOATING_POINT_SYNDROME_BASE);

#if UML_RUNTIME_PROFILING
enum class DispatchExit : std::size_t {
	system_call,
	memory_fault,
	interrupt,
	budget,
	illegal_instruction,
	breakpoint,
	floating_point_trap,
	pc_alignment,
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

struct ThreadCpuReading final {
	std::uint64_t nanoseconds;
	bool valid;
};

[[nodiscard]] ThreadCpuReading thread_cpu_reading() noexcept
{
	timespec value{};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0)
		return {
			.nanoseconds = 0,
			.valid = false,
		};
	return {
		.nanoseconds =
			static_cast<std::uint64_t>(value.tv_sec) *
				1'000'000'000ULL +
			static_cast<std::uint64_t>(value.tv_nsec),
		.valid = true,
	};
}

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
	static thread_local const auto cpu_origin = thread_cpu_reading();
	const auto elapsed = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			DispatchClock::now() - started).count());
	++statistics.calls;
	statistics.nanoseconds += elapsed;
	statistics.instructions += instructions;
	statistics.maximum_nanoseconds = std::max(
		statistics.maximum_nanoseconds, elapsed);
	++statistics.exits[static_cast<std::size_t>(reason)];
	// Keep full-guest profiling sparse enough that forwarding the diagnostic
	// through an iOS console cannot become part of the workload being measured.
	constexpr std::uint64_t report_interval = 4096;
	static_assert((report_interval & (report_interval - 1)) == 0);
	if ((statistics.calls & (report_interval - 1)) != 0)
		return;
	const auto current_cpu = thread_cpu_reading();
	const auto carrier_cpu_nanoseconds = cpu_origin.valid && current_cpu.valid &&
		current_cpu.nanoseconds >= cpu_origin.nanoseconds
		? current_cpu.nanoseconds - cpu_origin.nanoseconds
		: 0;
	std::array<char, 64> thread_name{};
	if (pthread_getname_np(pthread_self(), thread_name.data(),
		    thread_name.size()) != 0 || thread_name[0] == '\0')
		std::memcpy(thread_name.data(), "unnamed", sizeof("unnamed"));
	std::fprintf(stderr,
		"Darwin dispatch profile thread=%s calls=%llu nanoseconds=%llu "
		"carrier-cpu-nanoseconds=%llu instructions=%llu "
		"average-us=%.1f max-us=%.1f "
		"average-instructions=%.1f syscall=%llu fault=%llu interrupt=%llu "
		"budget=%llu illegal=%llu breakpoint=%llu fp-trap=%llu "
		"pc-alignment=%llu fatal=%llu\n",
		thread_name.data(),
		static_cast<unsigned long long>(statistics.calls),
		static_cast<unsigned long long>(statistics.nanoseconds),
		static_cast<unsigned long long>(carrier_cpu_nanoseconds),
		static_cast<unsigned long long>(statistics.instructions),
		statistics.nanoseconds / (statistics.calls * 1000.0),
		statistics.maximum_nanoseconds / 1000.0,
		statistics.instructions / static_cast<double>(statistics.calls),
		static_cast<unsigned long long>(statistics.exits[0]),
		static_cast<unsigned long long>(statistics.exits[1]),
		static_cast<unsigned long long>(statistics.exits[2]),
		static_cast<unsigned long long>(statistics.exits[3]),
		static_cast<unsigned long long>(statistics.exits[4]),
		static_cast<unsigned long long>(statistics.exits[5]),
		static_cast<unsigned long long>(statistics.exits[6]),
		static_cast<unsigned long long>(statistics.exits[7]),
		static_cast<unsigned long long>(statistics.exits[8]));
}
#endif

void ensure_pointer_auth_keys(
	darwin_uml_guest_registers &registers) noexcept
{
	bool uninitialized = true;
	for (const auto &key : registers.pointer_auth_keys)
		for (const auto half : key)
			uninitialized &= half == 0;
	if (!uninitialized)
		return;

	arc4random_buf(registers.pointer_auth_keys,
		       sizeof(registers.pointer_auth_keys));
	for (std::size_t index = 0;
	     index < DARWIN_UML_POINTER_AUTH_KEY_COUNT; ++index) {
		auto &low = registers.pointer_auth_keys[index][0];
		auto &high = registers.pointer_auth_keys[index][1];
		/*
		 * An all-zero 128-bit key is valid but is also the ABI's
		 * uninitialized marker. Avoid the vanishingly unlikely collision
		 * so that task migration cannot regenerate a key.
		 */
		if (low == 0 && high == 0) {
			low = arm64::deterministic_pointer_auth_keys[index].low;
			high = arm64::deterministic_pointer_auth_keys[index].high;
		}
	}
}

void load_cpu(arm64::CpuState &cpu,
	      darwin_uml_guest_registers &registers,
	      const darwin_uml_guest_fp_registers &fp, std::uint64_t tls,
	      bool top_byte_ignore)
{
	ensure_pointer_auth_keys(registers);
	cpu.pointer_authentication_enabled = false;
	cpu.tbi_enabled = top_byte_ignore;
	std::copy(std::begin(registers.x), std::end(registers.x),
		  cpu.registers.begin());
	cpu.registers[29] = registers.fp;
	cpu.registers[30] = registers.lr;
	cpu.sp = registers.sp;
	cpu.pc = registers.pc;
	cpu.nzcv = registers.pstate & pstate_nzcv;
	cpu.pstate_tco = (registers.pstate & pstate_tco) != 0;
	cpu.pstate_dit = (registers.pstate & pstate_dit) != 0;
	cpu.pstate_ssbs = (registers.pstate & pstate_ssbs) != 0;
	cpu.pstate_daif = static_cast<std::uint8_t>(
		(registers.pstate & pstate_daif) >> 6);
	cpu.pstate_sm = (registers.svcr & svcr_sm) != 0;
	cpu.pstate_za = (registers.svcr & svcr_za) != 0;
	cpu.fpmr = registers.fpmr;
	for (std::size_t index = 0;
	     index < DARWIN_UML_POINTER_AUTH_KEY_COUNT; ++index) {
		cpu.pointer_auth_keys[index].low =
			registers.pointer_auth_keys[index][0];
		cpu.pointer_auth_keys[index].high =
			registers.pointer_auth_keys[index][1];
	}
	cpu.tls = tls;
	/* An exception return may clear the local monitor, and the emulator has
	 * no architectural reservation token in uml_pt_regs.  Clear every
	 * diagnostic field so reusing the large CpuState scratch object cannot
	 * make a later STXR succeed spuriously.
	 */
	cpu.exclusive_address = ~std::uint64_t{0};
	cpu.exclusive_key = 0;
	cpu.exclusive_value = 0;
	cpu.exclusive_value2 = 0;
	cpu.exclusive_version = 0;
	cpu.exclusive_size = 0;
	cpu.gadget_zero = 0;
	cpu.gadget_indirect_miss = nullptr;
	cpu.gadget_host_fpmr = 0;
	cpu.gadget_fpmr_active = false;
	cpu.defer_fp_load(fp.vectors, fp.fpcr, fp.fpsr);
}

void save_cpu(arm64::CpuState &cpu,
	      darwin_uml_guest_registers &registers,
	      darwin_uml_guest_fp_registers &fp, unsigned long &tls)
{
	::uml::cpu::materialize_architectural_state(cpu);
	std::copy_n(cpu.registers.begin(), 29, std::begin(registers.x));
	registers.fp = cpu.registers[29];
	registers.lr = cpu.registers[30];
	registers.sp = cpu.sp;
	registers.pc = cpu.pc;
	registers.pstate =
		(registers.pstate &
		 ~(pstate_nzcv | pstate_tco | pstate_dit |
		   pstate_ssbs | pstate_daif)) |
		(cpu.nzcv & pstate_nzcv) |
		(cpu.pstate_tco ? pstate_tco : 0U) |
		(cpu.pstate_dit ? pstate_dit : 0U) |
		(cpu.pstate_ssbs ? pstate_ssbs : 0U) |
		(static_cast<std::uint32_t>(cpu.pstate_daif & 15U) << 6);
	registers.fpmr = cpu.fpmr;
	registers.svcr =
		(cpu.pstate_sm ? svcr_sm : 0U) |
		(cpu.pstate_za ? svcr_za : 0U);
	for (std::size_t index = 0;
	     index < DARWIN_UML_POINTER_AUTH_KEY_COUNT; ++index) {
		registers.pointer_auth_keys[index][0] =
			cpu.pointer_auth_keys[index].low;
		registers.pointer_auth_keys[index][1] =
			cpu.pointer_auth_keys[index].high;
	}
	tls = static_cast<unsigned long>(cpu.tls);
	if (cpu.fp_dirty) {
		std::memcpy(fp.vectors, cpu.vectors.data(), sizeof(fp.vectors));
		fp.fpsr = cpu.fpsr;
		fp.fpcr = cpu.fpcr;
	}
}

#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
void load_x86_cpu(x86_64::CpuState &cpu,
	const darwin_uml_guest_registers &registers,
	const darwin_uml_guest_fp_registers &fp) noexcept
{
	std::copy_n(std::begin(registers.x), x86_64_register_count,
		cpu.registers.begin());
	cpu.rip = registers.pc;
	cpu.rflags = registers.x[x86_64_rflags_slot] |
		x86_64::rflags_reserved;
	cpu.fs_base = registers.x[x86_64_fs_base_slot];
	cpu.gs_base = registers.x[x86_64_gs_base_slot];
	cpu.deferred_flags = {};
	if (fp.reserved[1] == x86_64_fp_marker) {
		static_assert(sizeof(cpu.vectors) == sizeof(fp.vectors));
		std::memcpy(cpu.vectors.data(), fp.vectors, sizeof(fp.vectors));
		cpu.mxcsr = fp.fpsr;
	} else {
		cpu.vectors = {};
		cpu.mmx = {};
		cpu.mxcsr = 0x1f80;
	}
}

void save_x86_cpu(x86_64::CpuState &cpu,
	darwin_uml_guest_registers &registers,
	darwin_uml_guest_fp_registers &fp) noexcept
{
	x86_64::materialize_flags(cpu);
	std::copy(cpu.registers.begin(), cpu.registers.end(),
		std::begin(registers.x));
	registers.pc = cpu.rip;
	registers.x[x86_64_rflags_slot] = cpu.rflags;
	registers.x[x86_64_fs_base_slot] = cpu.fs_base;
	registers.x[x86_64_gs_base_slot] = cpu.gs_base;
	static_assert(sizeof(cpu.vectors) == sizeof(fp.vectors));
	std::memcpy(fp.vectors, cpu.vectors.data(), sizeof(fp.vectors));
	fp.fpsr = cpu.mxcsr;
	fp.reserved[1] = x86_64_fp_marker;
}

[[nodiscard]] uml::cpu::ExecutionResult run_x86_gadgets(
	x86_64::CpuState &cpu, StaticGuestMemory &memory,
	unsigned long generation, const uml::cpu::InterruptToken &interrupt) noexcept
{
	memory.set_generation(generation);
	uml::cpu::ExecutionResult result{};
	result.reason = uml::cpu::ExitReason::fatal;
	while (result.retired_instructions < dispatch_budget) {
		if (cpu.rip == DARWIN_UML_SIGRETURN_TRAMPOLINE) {
			cpu.gpr(x86_64::Register::rax) =
				x86_64_rt_sigreturn;
			result.reason = uml::cpu::ExitReason::system_call;
			return result;
		}
		const auto built = x86_64::gadgets::build(memory, cpu.rip);
		switch (built.status) {
		case x86_64::gadgets::BuildStatus::fetch_fault:
			result.reason = uml::cpu::ExitReason::memory_fault;
			result.fault = {
				built.fault_address, uml::memory::MemoryAccess::execute};
			return result;
		case x86_64::gadgets::BuildStatus::illegal_instruction:
			result.reason = uml::cpu::ExitReason::illegal_instruction;
			return result;
		case x86_64::gadgets::BuildStatus::allocation_failure:
			return result;
		case x86_64::gadgets::BuildStatus::built:
			break;
		}
		const auto available = dispatch_budget - result.retired_instructions;
		const auto executed = x86_64::gadgets::execute(
			built.program, cpu, memory, available, interrupt);
		result.retired_instructions += executed.retired_instructions;
		result.fault = executed.fault;
		if (executed.instruction_budget_exhausted) {
			result.reason = uml::cpu::ExitReason::instruction_budget;
			return result;
		}
		switch (executed.event) {
		case uml::cpu::EngineEvent::continued:
			continue;
		case uml::cpu::EngineEvent::system_call:
			result.reason = uml::cpu::ExitReason::system_call;
			return result;
		case uml::cpu::EngineEvent::memory_fault:
			result.reason = uml::cpu::ExitReason::memory_fault;
			return result;
		case uml::cpu::EngineEvent::interrupt:
			result.reason = uml::cpu::ExitReason::interrupt;
			return result;
		case uml::cpu::EngineEvent::illegal_instruction:
			result.reason = uml::cpu::ExitReason::illegal_instruction;
			return result;
		case uml::cpu::EngineEvent::breakpoint:
			result.reason = uml::cpu::ExitReason::breakpoint;
			return result;
		case uml::cpu::EngineEvent::fatal:
		case uml::cpu::EngineEvent::gcs_fault:
		case uml::cpu::EngineEvent::floating_point_trap:
		case uml::cpu::EngineEvent::pc_alignment:
			return result;
		}
	}
	result.reason = uml::cpu::ExitReason::instruction_budget;
	return result;
}

template<uml::cpu::EngineKind Kind>
int run_x86_guest(darwin_uml_guest_registers &registers,
	darwin_uml_guest_fp_registers &fp,
	unsigned long generation, ::uml::runtime::Context &context,
	::uml::runtime::ThreadExecutionState &runtime) noexcept
{
	(void)Kind;
	auto &cpu = runtime.x86_cpu;
	load_x86_cpu(cpu, registers, fp);
	const auto &registered_interrupt = runtime.interrupt;
	const auto result = run_x86_gadgets(cpu, runtime.memory, generation, {
		.sequence = registered_interrupt.sequence,
		.snapshot = registered_interrupt.snapshot,
		.stop = context.stop_token(),
	});
	save_x86_cpu(cpu, registers, fp);
	int event = DARWIN_UML_EVENT_FATAL;
	switch (result.reason) {
	case uml::cpu::ExitReason::system_call:
		event = DARWIN_UML_EVENT_SYSCALL;
		break;
	case uml::cpu::ExitReason::memory_fault:
		registers.fault.error_code = result.fault.access ==
			uml::memory::MemoryAccess::write ? DARWIN_UML_FAULT_WRITE :
			result.fault.access == uml::memory::MemoryAccess::execute ?
				DARWIN_UML_FAULT_EXECUTE : 0;
		registers.fault.address = result.fault.address;
		registers.fault.trap_number = DARWIN_UML_FAULT_PAGE;
		event = DARWIN_UML_EVENT_FAULT;
		break;
	case uml::cpu::ExitReason::interrupt:
	case uml::cpu::ExitReason::instruction_budget:
		event = context.stop_requested() ?
			DARWIN_UML_EVENT_STOP : DARWIN_UML_EVENT_INTERRUPT;
		break;
	case uml::cpu::ExitReason::illegal_instruction:
		event = DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION;
		break;
	case uml::cpu::ExitReason::breakpoint:
		event = DARWIN_UML_EVENT_BREAKPOINT;
		break;
	case uml::cpu::ExitReason::gcs_fault:
	case uml::cpu::ExitReason::floating_point_trap:
	case uml::cpu::ExitReason::pc_alignment:
	case uml::cpu::ExitReason::fatal:
		break;
	}
	return event;
}
#endif

void save_floating_point_trap(
	darwin_uml_guest_registers &registers, std::uint64_t address,
	std::uint32_t syndrome) noexcept
{
	registers.fault.error_code = static_cast<std::int32_t>(syndrome);
	registers.fault.address = address;
	registers.fault.trap_number = 0;
}

[[nodiscard]] bool prepare_signal_return(arm64::CpuState &cpu) noexcept
{
	if (cpu.pc != DARWIN_UML_SIGRETURN_TRAMPOLINE)
		return false;
	cpu.registers[8] = DARWIN_UML_NR_RT_SIGRETURN;
	cpu.pc += sizeof(std::uint32_t);
	return true;
}

class InterpreterExecutionScope final {
public:
	InterpreterExecutionScope(unsigned long generation, std::uint64_t guest_pc)
		: generation_{generation},
		  context_{
			darwin_uml_static_vm_execution_context_create(generation)}
	{
		entered_ = context_ != nullptr &&
			darwin_uml_static_vm_execution_context_begin(
				context_, guest_pc) == 0;
	}

	~InterpreterExecutionScope()
	{
		if (entered_)
			(void)darwin_uml_static_vm_execution_context_end(context_);
		darwin_uml_static_vm_execution_context_destroy(context_);
	}

	InterpreterExecutionScope(const InterpreterExecutionScope &) = delete;
	InterpreterExecutionScope &operator=(const InterpreterExecutionScope &) =
		delete;

	[[nodiscard]] bool entered() const noexcept { return entered_; }
	[[nodiscard]] darwin_uml_static_sequences sequences() const noexcept
	{
		return darwin_uml_static_vm_execution_context_sequences(context_);
	}
	[[nodiscard]] unsigned long generation() const noexcept
	{
		return generation_;
	}

private:
	unsigned long generation_;
	void *context_{};
	bool entered_{};
};

template<uml::cpu::EngineKind Kind>
std::unique_ptr<uml::cpu::CpuEngine> &engine_slot(
	::uml::runtime::ThreadExecutionState &runtime)
{
	return runtime.engines.slot<Kind>();
}

template<uml::cpu::EngineKind Kind>
uml::cpu::CpuEngine *common_engine(
	::uml::runtime::ThreadExecutionState &runtime)
{
	auto &slot = engine_slot<Kind>(runtime);
	if (!slot)
		slot = uml::cpu::make_cpu_engine(Kind,
			uml::cpu::EngineConfiguration{
				.enable_guarded_control_flow = false,
				.enable_pointer_authentication = false,
				.host_capabilities =
					::uml::platform::apple::probed_host_capabilities(),
			});
	return slot.get();
}

bool execution_interrupt_boundary(void *context) noexcept
{
	const auto *execution =
		static_cast<const InterpreterExecutionScope *>(context);
	return execution != nullptr &&
		execution->sequences().execution_interrupt != 0;
}

bool signal_return_boundary(arm64::CpuState &cpu, void *) noexcept
{
	return prepare_signal_return(cpu);
}

#if UML_RUNTIME_PROFILING
[[nodiscard]] DispatchExit dispatch_exit_for(
	uml::cpu::ExitReason reason) noexcept
{
	switch (reason) {
	case uml::cpu::ExitReason::system_call:
		return DispatchExit::system_call;
	case uml::cpu::ExitReason::memory_fault:
		return DispatchExit::memory_fault;
	case uml::cpu::ExitReason::interrupt:
		return DispatchExit::interrupt;
	case uml::cpu::ExitReason::illegal_instruction:
		return DispatchExit::illegal_instruction;
	case uml::cpu::ExitReason::breakpoint:
		return DispatchExit::breakpoint;
	case uml::cpu::ExitReason::gcs_fault:
		return DispatchExit::memory_fault;
	case uml::cpu::ExitReason::floating_point_trap:
		return DispatchExit::floating_point_trap;
	case uml::cpu::ExitReason::pc_alignment:
		return DispatchExit::pc_alignment;
	case uml::cpu::ExitReason::instruction_budget:
		return DispatchExit::budget;
	case uml::cpu::ExitReason::fatal:
		return DispatchExit::fatal;
	}
	return DispatchExit::fatal;
}
#endif

template<uml::cpu::EngineKind Kind>
int run_common_guest(
	void *register_pointer, void *fp_pointer,
	unsigned long fp_size, unsigned long generation, unsigned long *tls)
{
	if (register_pointer == nullptr || fp_pointer == nullptr || tls == nullptr ||
		fp_size < sizeof(darwin_uml_guest_fp_registers))
		return DARWIN_UML_EVENT_FATAL;
	auto &mutable_registers =
		*static_cast<darwin_uml_guest_registers *>(register_pointer);
	auto &fp = *static_cast<darwin_uml_guest_fp_registers *>(fp_pointer);
	auto *context = ::uml::runtime::current_context();
	if (context == nullptr)
		return DARWIN_UML_EVENT_FATAL;
	auto runtime = context->thread_execution_state();
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	if (mutable_registers.padding == x86_64_guest_isa) {
		return run_x86_guest<Kind>(mutable_registers, fp, generation,
			*context, *runtime);
	}
#endif
	if constexpr (Kind == uml::cpu::EngineKind::jit) {
		if (!uml::cpu::cpu_engine_available(uml::cpu::EngineKind::jit))
			return DARWIN_UML_EVENT_FATAL;
	}
	const auto &registers = *static_cast<const darwin_uml_guest_registers *>(
		register_pointer);
	if (registers.pc == DARWIN_UML_SIGRETURN_TRAMPOLINE) {
		auto &mutable_registers =
			*static_cast<darwin_uml_guest_registers *>(register_pointer);
		mutable_registers.x[8] = DARWIN_UML_NR_RT_SIGRETURN;
		mutable_registers.pc += sizeof(std::uint32_t);
		return DARWIN_UML_EVENT_SYSCALL;
	}
	auto &cpu = runtime->cpu;
	auto &memory = runtime->memory;
	load_cpu(cpu, mutable_registers, fp, *tls, context->top_byte_ignore());
	if constexpr (Kind != uml::cpu::EngineKind::gadgets)
		cpu.access_fp_state();
	auto *engine = common_engine<Kind>(*runtime);
	if (engine == nullptr)
		return DARWIN_UML_EVENT_FATAL;

#if UML_RUNTIME_PROFILING
	const auto started = DispatchClock::now();
#endif
	const auto &interrupt = runtime->interrupt;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = generation,
		.instruction_budget = dispatch_budget,
		.interrupt = {
			.sequence = interrupt.sequence,
			.snapshot = interrupt.snapshot,
			.stop = context->stop_token(),
		},
		.boundary = signal_return_boundary,
	};
	int event = DARWIN_UML_EVENT_FATAL;
	uml::cpu::ExecutionResult result{};
	if constexpr (Kind == uml::cpu::EngineKind::interpreter) {
		InterpreterExecutionScope execution{generation, cpu.pc};
		if (!execution.entered())
			return DARWIN_UML_EVENT_FATAL;
		const auto sequences = execution.sequences();
		memory.prepare(generation, sequences.mapping_changes);
		request.boundary_context = &execution;
		request.interrupt.poll = execution_interrupt_boundary;
		request.interrupt.poll_context = &execution;
		result = engine->run(request);
	} else {
		result = engine->run(request);
	}
	save_cpu(cpu, mutable_registers, fp, *tls);

	switch (result.reason) {
	case uml::cpu::ExitReason::system_call:
		event = DARWIN_UML_EVENT_SYSCALL;
		break;
	case uml::cpu::ExitReason::memory_fault:
		switch (result.fault.access) {
		case uml::memory::MemoryAccess::read:
			mutable_registers.fault.error_code = 0;
			break;
		case uml::memory::MemoryAccess::write:
			mutable_registers.fault.error_code = DARWIN_UML_FAULT_WRITE;
			break;
		case uml::memory::MemoryAccess::execute:
			mutable_registers.fault.error_code = DARWIN_UML_FAULT_EXECUTE;
			break;
		}
		mutable_registers.fault.address = result.fault.address;
		mutable_registers.fault.trap_number = DARWIN_UML_FAULT_PAGE;
		event = DARWIN_UML_EVENT_FAULT;
		break;
	case uml::cpu::ExitReason::interrupt:
		if (context->stop_requested()) {
			event = DARWIN_UML_EVENT_STOP;
			break;
		}
		[[fallthrough]];
	case uml::cpu::ExitReason::instruction_budget:
		event = DARWIN_UML_EVENT_INTERRUPT;
		break;
	case uml::cpu::ExitReason::illegal_instruction:
		event = DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION;
		break;
	case uml::cpu::ExitReason::breakpoint:
		event = DARWIN_UML_EVENT_BREAKPOINT;
		break;
	case uml::cpu::ExitReason::gcs_fault:
		event = DARWIN_UML_EVENT_GCS_FAULT;
		break;
	case uml::cpu::ExitReason::floating_point_trap:
		save_floating_point_trap(
			mutable_registers, cpu.pc, result.syndrome);
		event = DARWIN_UML_EVENT_FLOATING_POINT_TRAP;
		break;
	case uml::cpu::ExitReason::pc_alignment:
		mutable_registers.fault.error_code = 0;
		mutable_registers.fault.address = result.fault.address;
		mutable_registers.fault.trap_number = 0;
		event = DARWIN_UML_EVENT_PC_ALIGNMENT;
		break;
	case uml::cpu::ExitReason::fatal:
		event = DARWIN_UML_EVENT_FATAL;
		break;
	}
#if UML_RUNTIME_PROFILING
	profile_dispatch(started, result.retired_instructions,
		dispatch_exit_for(result.reason));
#endif
	return event;
}

} // namespace
} // namespace uml::emulator

extern "C" int darwin_uml_static_guest_interrupt_sequence(
	const unsigned int *sequence, unsigned int snapshot)
{
	try {
		auto *context = ::uml::runtime::current_context();
		if (context == nullptr)
			return -1;
		auto runtime = context->thread_execution_state();
		runtime->interrupt.sequence = sequence;
		runtime->interrupt.snapshot = snapshot;
		return 0;
	} catch (...) {
		/* Registration is a checked transaction. Linux must not enter the
		 * guest after this failure, and an explicit retry replaces any prior
		 * token before execution can resume. */
		return -1;
	}
}

extern "C" int darwin_uml_interpreter_enter_guest(
	void *register_pointer, void *fp_pointer, unsigned long fp_size,
	unsigned long generation, unsigned long *tls)
{
	try {
		return uml::emulator::run_common_guest<
			uml::cpu::EngineKind::interpreter>(register_pointer, fp_pointer,
			fp_size, generation, tls);
	} catch (...) {
		return DARWIN_UML_EVENT_FATAL;
	}
}

extern "C" int darwin_uml_jit_enter_guest(
	void *register_pointer, void *fp_pointer, unsigned long fp_size,
	unsigned long generation, unsigned long *tls)
{
	try {
		return uml::emulator::run_common_guest<
			uml::cpu::EngineKind::jit>(register_pointer, fp_pointer,
			fp_size, generation, tls);
	} catch (...) {
		return DARWIN_UML_EVENT_FATAL;
	}
}

extern "C" int darwin_uml_static_enter_guest(
	void *register_pointer, void *fp_pointer, unsigned long fp_size,
	unsigned long generation, unsigned long *tls)
{
	try {
		return uml::emulator::run_common_guest<
			uml::cpu::EngineKind::gadgets>(register_pointer, fp_pointer,
			fp_size, generation, tls);
	} catch (...) {
		return DARWIN_UML_EVENT_FATAL;
	}
}
