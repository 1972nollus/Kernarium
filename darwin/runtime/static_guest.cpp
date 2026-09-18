// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory/static_guest_memory.hpp"
#include "cpu/engine_factory.hpp"
#if DARWIN_UML_X86_64_GUEST
#include "runtime/x86_gadget_runtime.hpp"
#endif
#if DARWIN_UML_WINE_CPU_SERVICE
#include "runtime/wine_cpu_service.hpp"
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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#if DARWIN_UML_X86_64_GUEST
namespace x86_64 = ::uml::cpu::x86_64;
#endif
using ::uml::memory::StaticGuestMemory;
namespace {

#if DARWIN_UML_X86_64_GUEST && !DARWIN_UML_X86_64_USERSPACE_ONLY
constexpr std::int32_t x86_64_guest_isa = 2;
#endif
#if DARWIN_UML_X86_64_GUEST
constexpr std::uint32_t x86_64_fp_marker = 0x5838'3634U;
constexpr std::size_t x86_64_register_count = 16;
constexpr std::size_t x86_64_rflags_slot = 16;
constexpr std::size_t x86_64_fs_base_slot = 17;
constexpr std::size_t x86_64_gs_base_slot = 18;
constexpr std::size_t x86_64_x87_significand_slot = 19;
constexpr std::size_t x86_64_x87_exponent_slot = 27;
constexpr std::size_t x86_32_tls_entry_count = 3;
constexpr std::uint64_t x86_64_compatibility_syscall = 1ULL << 32U;
constexpr std::uint64_t x86_64_compat32_execution = 1ULL << 33U;
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

#if DARWIN_UML_X86_64_GUEST
[[nodiscard]] bool x86_compat_syscall_trace_enabled() noexcept
{
	static const bool enabled = [] {
		const auto *value = std::getenv("UML_X86_COMPAT_SYSCALL_TRACE");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return enabled;
}

[[nodiscard]] bool x86_write_trace_enabled() noexcept
{
	static const bool enabled = [] {
		const auto *value = std::getenv("UML_X86_WRITE_TRACE");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return enabled;
}

struct X86Iovec final {
	std::uint64_t base;
	std::uint64_t length;
};

void trace_x86_write(const x86_64::CpuState &cpu,
	StaticGuestMemory &memory, unsigned long generation) noexcept
{
	const auto number = cpu.gpr(x86_64::Register::rax);
	if (cpu.execution_mode == x86_64::ExecutionMode::compat32 &&
		cpu.system_call_32bit && (x86_compat_syscall_trace_enabled() ||
			x86_write_trace_enabled())) {
		static std::atomic_uint64_t compat_sequence{};
		const auto current = compat_sequence.fetch_add(
			1, std::memory_order_relaxed) + 1;
		std::fprintf(stderr,
			"Darwin i386 syscall trace sequence=%llu generation=%lu "
			"nr=%u rip=%#llx args=%#x,%#x,%#x,%#x,%#x,%#x\n",
			static_cast<unsigned long long>(current), generation,
			static_cast<unsigned>(number),
			static_cast<unsigned long long>(cpu.rip),
			static_cast<unsigned>(cpu.gpr(x86_64::Register::rbx)),
			static_cast<unsigned>(cpu.gpr(x86_64::Register::rcx)),
			static_cast<unsigned>(cpu.gpr(x86_64::Register::rdx)),
			static_cast<unsigned>(cpu.gpr(x86_64::Register::rsi)),
			static_cast<unsigned>(cpu.gpr(x86_64::Register::rdi)),
			static_cast<unsigned>(cpu.gpr(x86_64::Register::rbp)));
		std::fflush(stderr);
		return;
	}
	if (!x86_write_trace_enabled())
		return;
	if (number != 1 && number != 20)
		return;
	static std::atomic_uint64_t sequence{};
	const auto current = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	const auto descriptor = cpu.gpr(x86_64::Register::rdi);
	const auto address = cpu.gpr(x86_64::Register::rsi);
	const auto third = cpu.gpr(x86_64::Register::rdx);
	std::fprintf(stderr,
		"Darwin x86 write trace sequence=%llu generation=%lu nr=%llu "
		"fd=%llu address=%#llx third=%llu rip=%#llx",
		static_cast<unsigned long long>(current), generation,
		static_cast<unsigned long long>(number),
		static_cast<unsigned long long>(descriptor),
		static_cast<unsigned long long>(address),
		static_cast<unsigned long long>(third),
		static_cast<unsigned long long>(cpu.rip));
	if (number == 1) {
		std::array<std::byte, 256> sample{};
		const auto sample_size = std::min<std::uint64_t>(third, sample.size());
		const bool readable = sample_size == 0 || memory.read(
			address, std::span{sample}.first(
				static_cast<std::size_t>(sample_size)));
		const auto zeros = readable ? std::count(
			sample.begin(), sample.begin() + static_cast<std::ptrdiff_t>(sample_size),
			std::byte{}) : 0;
		std::fprintf(stderr, " sample=%llu zeros=%zu readable=%u",
			static_cast<unsigned long long>(sample_size),
			static_cast<std::size_t>(zeros), readable ? 1U : 0U);
	} else {
		constexpr std::size_t maximum_iovecs = 16;
		std::array<X86Iovec, maximum_iovecs> vectors{};
		const auto count = std::min<std::uint64_t>(third, vectors.size());
		const bool readable = count == 0 || memory.read(address,
			std::as_writable_bytes(std::span{vectors}.first(
				static_cast<std::size_t>(count))));
		std::uint64_t total{};
		std::uint64_t sampled{};
		std::size_t zeros{};
		if (readable) {
			for (std::size_t index = 0; index < count; ++index) {
				total += vectors[index].length;
				std::array<std::byte, 64> sample{};
				const auto sample_size = std::min<std::uint64_t>(
					vectors[index].length, sample.size());
				if (sample_size != 0 && memory.read(vectors[index].base,
					std::span{sample}.first(static_cast<std::size_t>(sample_size)))) {
					sampled += sample_size;
					zeros += static_cast<std::size_t>(std::count(
						sample.begin(),
						sample.begin() + static_cast<std::ptrdiff_t>(sample_size),
						std::byte{}));
				}
			}
		}
		std::fprintf(stderr,
			" iovecs=%llu inspected=%llu total=%llu sample=%llu zeros=%zu readable=%u",
			static_cast<unsigned long long>(third),
			static_cast<unsigned long long>(count),
			static_cast<unsigned long long>(total),
			static_cast<unsigned long long>(sampled), zeros,
			readable ? 1U : 0U);
	}
	std::fputc('\n', stderr);
	std::fflush(stderr);
}

void load_x86_cpu(x86_64::CpuState &cpu,
	const darwin_uml_guest_registers &registers,
	const darwin_uml_guest_fp_registers &fp) noexcept
{
	std::copy_n(std::begin(registers.x), x86_64_register_count,
		cpu.registers.begin());
	cpu.execution_mode =
		(registers.svcr & x86_64_compat32_execution) != 0 ?
			x86_64::ExecutionMode::compat32 : x86_64::ExecutionMode::long64;
	cpu.rip = x86_64::normalize_instruction_pointer(
		cpu.execution_mode, registers.pc);
	cpu.rflags = registers.x[x86_64_rflags_slot] |
		x86_64::rflags_reserved;
	cpu.compat_tls_bases = {};
	cpu.compat_tls_present = 0;
	if (cpu.execution_mode == x86_64::ExecutionMode::compat32) {
		for (std::size_t index = 0; index < x86_32_tls_entry_count; ++index) {
			const auto head = registers.pointer_auth_keys[index][0];
			const auto tail = registers.pointer_auth_keys[index][1];
			if ((head | tail) == 0)
				continue;
			cpu.compat_tls_bases[index] = static_cast<std::uint32_t>(head >> 32U);
			cpu.compat_tls_present |= static_cast<std::uint8_t>(1U << index);
		}
	}
	cpu.fs_base = registers.x[x86_64_fs_base_slot];
	cpu.gs_base = registers.x[x86_64_gs_base_slot];
	cpu.deferred_flags = {};
	if (fp.reserved[1] == x86_64_fp_marker) {
		static_assert(sizeof(cpu.vectors) == sizeof(fp.vectors));
		std::memcpy(cpu.vectors.data(), fp.vectors, sizeof(fp.vectors));
		cpu.mxcsr = fp.fpsr;
		cpu.x87_control = static_cast<std::uint16_t>(fp.fpcr);
		cpu.x87_status = static_cast<std::uint16_t>(fp.fpcr >> 16U);
		cpu.x87_tag = static_cast<std::uint16_t>(fp.reserved[0]);
		cpu.x87_top = static_cast<std::uint8_t>(fp.reserved[0] >> 16U);
		for (std::size_t index = 0; index < cpu.x87.size(); ++index) {
			cpu.x87[index].significand =
				registers.x[x86_64_x87_significand_slot + index];
			const auto packed = registers.x[
				x86_64_x87_exponent_slot + index / 4U];
			cpu.x87[index].sign_exponent = static_cast<std::uint16_t>(
				packed >> ((index % 4U) * 16U));
			cpu.mmx[index] = cpu.x87[index].significand;
		}
		for (std::size_t index = 0; index < 4; ++index)
			cpu.segment_selectors[index] = static_cast<std::uint16_t>(
				registers.fpmr >> (index * 16U));
		for (std::size_t index = 4; index < 6; ++index)
			cpu.segment_selectors[index] = static_cast<std::uint16_t>(
				registers.svcr >> ((index - 4U) * 16U));
		cpu.system_call_32bit =
			(registers.svcr & x86_64_compatibility_syscall) != 0;
	} else {
		cpu.vectors = {};
		cpu.mmx = {};
		cpu.x87 = {};
		cpu.mxcsr = 0x1f80;
		cpu.x87_control = 0x037f;
		cpu.x87_status = 0;
		cpu.x87_tag = 0xffff;
		cpu.x87_top = 0;
		cpu.segment_selectors = {0, static_cast<std::uint16_t>(
			cpu.execution_mode == x86_64::ExecutionMode::compat32 ? 0x23 : 0x33),
			0x2b, 0, 0, 0};
		cpu.system_call_32bit = false;
	}
}

void save_x86_cpu(x86_64::CpuState &cpu,
	darwin_uml_guest_registers &registers,
	darwin_uml_guest_fp_registers &fp) noexcept
{
	x86_64::materialize_flags(cpu);
	std::copy(cpu.registers.begin(), cpu.registers.end(),
		std::begin(registers.x));
	registers.pc = x86_64::normalize_instruction_pointer(
		cpu.execution_mode, cpu.rip);
	registers.x[x86_64_rflags_slot] = cpu.rflags;
	registers.x[x86_64_fs_base_slot] = cpu.fs_base;
	registers.x[x86_64_gs_base_slot] = cpu.gs_base;
	for (std::size_t index = 0; index < cpu.x87.size(); ++index)
		registers.x[x86_64_x87_significand_slot + index] =
			cpu.x87[index].significand;
	for (std::size_t group = 0; group < 2; ++group) {
		std::uint64_t packed{};
		for (std::size_t index = 0; index < 4; ++index)
			packed |= static_cast<std::uint64_t>(
				cpu.x87[group * 4U + index].sign_exponent) <<
				(index * 16U);
		registers.x[x86_64_x87_exponent_slot + group] = packed;
	}
	registers.fpmr = 0;
	for (std::size_t index = 0; index < 4; ++index)
		registers.fpmr |= static_cast<std::uint64_t>(
			cpu.segment_selectors[index]) << (index * 16U);
	registers.svcr = static_cast<std::uint64_t>(cpu.segment_selectors[4]) |
		(static_cast<std::uint64_t>(cpu.segment_selectors[5]) << 16U) |
		(cpu.system_call_32bit ? x86_64_compatibility_syscall : 0U) |
		(cpu.execution_mode == x86_64::ExecutionMode::compat32 ?
			x86_64_compat32_execution : 0U);
	static_assert(sizeof(cpu.vectors) == sizeof(fp.vectors));
	std::memcpy(fp.vectors, cpu.vectors.data(), sizeof(fp.vectors));
	fp.fpsr = cpu.mxcsr;
	fp.fpcr = static_cast<std::uint32_t>(cpu.x87_control) |
		(static_cast<std::uint32_t>(cpu.x87_status) << 16U);
	fp.reserved[0] = static_cast<std::uint32_t>(cpu.x87_tag) |
		(static_cast<std::uint32_t>(cpu.x87_top) << 16U);
	fp.reserved[1] = x86_64_fp_marker;
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
	const auto run = ::uml::runtime::run_x86_gadgets(
		cpu, runtime.memory, runtime.x86_executor, generation,
		dispatch_budget, {
		.sequence = registered_interrupt.sequence,
		.snapshot = registered_interrupt.snapshot,
		.stop = context.stop_token(),
	});
	const auto &result = run.execution;
	if (result.reason == uml::cpu::ExitReason::system_call)
		trace_x86_write(cpu, runtime.memory, generation);
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
		registers.fault.error_code = 0;
		registers.fault.address = cpu.rip;
		registers.fault.trap_number = static_cast<std::int32_t>(
			result.syndrome);
		event = DARWIN_UML_EVENT_BREAKPOINT;
		break;
	case uml::cpu::ExitReason::floating_point_trap:
		registers.fault.error_code = 0;
		registers.fault.address = cpu.rip;
		registers.fault.trap_number = 0;
		event = DARWIN_UML_EVENT_FLOATING_POINT_TRAP;
		break;
	case uml::cpu::ExitReason::gcs_fault:
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
#if DARWIN_UML_X86_64_USERSPACE_ONLY
	return run_x86_guest<Kind>(mutable_registers, fp, generation,
		*context, *runtime);
#elif DARWIN_UML_X86_64_GUEST
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
	const uml::cpu::InterruptToken dispatch_interrupt{
		.sequence = interrupt.sequence,
		.snapshot = interrupt.snapshot,
		.stop = context->stop_token(),
	};
	int event = DARWIN_UML_EVENT_FATAL;
	uml::cpu::ExecutionResult result{};
	std::size_t retired_instructions{};
	for (;;) {
		uml::cpu::ExecutionRequest request{
			.cpu = cpu,
			.memory = memory,
			.translated_memory = &memory,
			.generation = generation,
			.instruction_budget = dispatch_budget - retired_instructions,
			.interrupt = dispatch_interrupt,
			.boundary = signal_return_boundary,
		};
		result = engine->run(request);
		retired_instructions += result.retired_instructions;
		if (result.reason != uml::cpu::ExitReason::breakpoint)
			break;
#if DARWIN_UML_WINE_CPU_SERVICE
		const auto hostcall = ::uml::runtime::handle_wine_cpu_hostcall(
			cpu, *runtime, generation,
			dispatch_budget - retired_instructions, dispatch_interrupt);
		switch (hostcall.disposition) {
		case ::uml::runtime::WineCpuHostcallDisposition::not_handled:
			break;
		case ::uml::runtime::WineCpuHostcallDisposition::memory_fault:
			result.reason = uml::cpu::ExitReason::memory_fault;
			result.fault = hostcall.fault;
			break;
		case ::uml::runtime::WineCpuHostcallDisposition::resume:
			retired_instructions += hostcall.retired_instructions;
			if (retired_instructions < dispatch_budget)
				continue;
			result.reason = uml::cpu::ExitReason::instruction_budget;
			break;
		}
#endif
		break;
	}
	result.retired_instructions = retired_instructions;
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
