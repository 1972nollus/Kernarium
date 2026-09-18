// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/wine_cpu_service.hpp"

#if DARWIN_UML_WINE_CPU_SERVICE

#include "runtime/wine_cpu_protocol.h"
#include "runtime/x86_gadget_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <span>

namespace uml::runtime {
namespace {

namespace arm64 = ::uml::cpu::arm64;
namespace x86_64 = ::uml::cpu::x86_64;
using ::uml::memory::MemoryAccess;
using ::uml::memory::StaticGuestMemory;

constexpr std::uint64_t capabilities =
	UML_WINE_CPU_CAP_X86_64 |
	UML_WINE_CPU_CAP_COMPAT32 |
	UML_WINE_CPU_CAP_AVX |
	UML_WINE_CPU_CAP_AVX2 |
	UML_WINE_CPU_CAP_X87 |
	UML_WINE_CPU_CAP_ARM64EC_BOUNDARY |
	UML_WINE_CPU_CAP_STRICT_TSO;

[[nodiscard]] std::uint32_t load_u32(
	const std::array<std::byte, 4> &bytes) noexcept
{
	return std::to_integer<std::uint32_t>(bytes[0]) |
		(std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
		(std::to_integer<std::uint32_t>(bytes[2]) << 16U) |
		(std::to_integer<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] bool read_request(StaticGuestMemory &memory,
	std::uint64_t address, uml_wine_cpu_request &request) noexcept
{
	return memory.read(address,
		std::as_writable_bytes(std::span{&request, std::size_t{1}}));
}

[[nodiscard]] bool write_request(StaticGuestMemory &memory,
	std::uint64_t address, const uml_wine_cpu_request &request) noexcept
{
	return memory.write(address,
		std::as_bytes(std::span{&request, std::size_t{1}}));
}

void load_context(x86_64::CpuState &cpu,
	const uml_wine_x86_context &context) noexcept
{
	std::copy(std::begin(context.gpr), std::end(context.gpr),
		cpu.registers.begin());
	cpu.rip = context.rip;
	cpu.rflags = context.rflags | x86_64::rflags_reserved;
	cpu.fs_base = context.fs_base;
	cpu.gs_base = context.gs_base;
	std::memcpy(cpu.vectors.data(), context.vectors,
		sizeof(context.vectors));
	std::copy(std::begin(context.mmx), std::end(context.mmx),
		cpu.mmx.begin());
	for (std::size_t index = 0; index < cpu.x87.size(); ++index) {
		cpu.x87[index].significand = context.x87_significand[index];
		cpu.x87[index].sign_exponent = context.x87_sign_exponent[index];
	}
	std::copy(std::begin(context.compat_tls_bases),
		std::end(context.compat_tls_bases), cpu.compat_tls_bases.begin());
	std::copy(std::begin(context.segment_selectors),
		std::end(context.segment_selectors), cpu.segment_selectors.begin());
	cpu.mxcsr = context.mxcsr;
	cpu.execution_mode = context.execution_mode == 1 ?
		x86_64::ExecutionMode::compat32 : x86_64::ExecutionMode::long64;
	cpu.x87_control = context.x87_control;
	cpu.x87_status = context.x87_status;
	cpu.x87_tag = context.x87_tag;
	cpu.x87_top = context.x87_top;
	cpu.x87_reduced_precision = context.x87_reduced_precision != 0;
	cpu.system_call_32bit = context.system_call_32bit != 0;
	cpu.compat_tls_present = context.compat_tls_present;
	cpu.segment_zero = 0;
	cpu.deferred_flags = {};
}

void save_context(x86_64::CpuState &cpu,
	uml_wine_x86_context &context) noexcept
{
	x86_64::materialize_flags(cpu);
	std::copy(cpu.registers.begin(), cpu.registers.end(),
		std::begin(context.gpr));
	context.rip = cpu.rip;
	context.rflags = cpu.rflags;
	context.fs_base = cpu.fs_base;
	context.gs_base = cpu.gs_base;
	std::memcpy(context.vectors, cpu.vectors.data(),
		sizeof(context.vectors));
	std::copy(cpu.mmx.begin(), cpu.mmx.end(), std::begin(context.mmx));
	for (std::size_t index = 0; index < cpu.x87.size(); ++index) {
		context.x87_significand[index] = cpu.x87[index].significand;
		context.x87_sign_exponent[index] = cpu.x87[index].sign_exponent;
	}
	std::copy(cpu.compat_tls_bases.begin(), cpu.compat_tls_bases.end(),
		std::begin(context.compat_tls_bases));
	std::copy(cpu.segment_selectors.begin(), cpu.segment_selectors.end(),
		std::begin(context.segment_selectors));
	context.mxcsr = cpu.mxcsr;
	context.execution_mode = cpu.execution_mode ==
		x86_64::ExecutionMode::compat32 ? 1U : 0U;
	context.x87_control = cpu.x87_control;
	context.x87_status = cpu.x87_status;
	context.x87_tag = cpu.x87_tag;
	context.x87_top = cpu.x87_top;
	context.x87_reduced_precision = cpu.x87_reduced_precision ? 1U : 0U;
	context.system_call_32bit = cpu.system_call_32bit ? 1U : 0U;
	context.compat_tls_present = cpu.compat_tls_present;
}

struct EcBitmap final {
	std::uint64_t address;
};

[[nodiscard]] bool is_native_target(StaticGuestMemory &memory,
	std::uint64_t address, void *opaque) noexcept
{
	const auto bitmap = static_cast<const EcBitmap *>(opaque)->address;
	const auto word_index = address >> 18U;
	if (word_index >
		(std::numeric_limits<std::uint64_t>::max() - bitmap) / 8U)
		return false;
	std::uint64_t word{};
	if (!memory.read(bitmap + word_index * 8U,
		std::as_writable_bytes(std::span{&word, std::size_t{1}})))
		return false;
	return ((word >> ((address >> 12U) & 63U)) & 1U) != 0;
}

[[nodiscard]] std::uint32_t stop_reason_for(
	const X86GadgetRunResult &run) noexcept
{
	if (run.native_target)
		return UML_WINE_CPU_STOP_NATIVE_TARGET;
	switch (run.execution.reason) {
	case ::uml::cpu::ExitReason::system_call:
		return UML_WINE_CPU_STOP_SYSTEM_CALL;
	case ::uml::cpu::ExitReason::memory_fault:
		return UML_WINE_CPU_STOP_MEMORY_FAULT;
	case ::uml::cpu::ExitReason::interrupt:
		return UML_WINE_CPU_STOP_INTERRUPT;
	case ::uml::cpu::ExitReason::illegal_instruction:
		return UML_WINE_CPU_STOP_ILLEGAL_INSTRUCTION;
	case ::uml::cpu::ExitReason::breakpoint:
		return UML_WINE_CPU_STOP_BREAKPOINT;
	case ::uml::cpu::ExitReason::floating_point_trap:
		return UML_WINE_CPU_STOP_FLOATING_POINT;
	case ::uml::cpu::ExitReason::instruction_budget:
		return UML_WINE_CPU_STOP_BUDGET;
	case ::uml::cpu::ExitReason::gcs_fault:
	case ::uml::cpu::ExitReason::pc_alignment:
	case ::uml::cpu::ExitReason::fatal:
		return UML_WINE_CPU_STOP_FATAL;
	}
	return UML_WINE_CPU_STOP_FATAL;
}

[[nodiscard]] std::uint32_t access_for(MemoryAccess access) noexcept
{
	switch (access) {
	case MemoryAccess::read:
		return UML_WINE_CPU_ACCESS_READ;
	case MemoryAccess::write:
		return UML_WINE_CPU_ACCESS_WRITE;
	case MemoryAccess::execute:
		return UML_WINE_CPU_ACCESS_EXECUTE;
	}
	return UML_WINE_CPU_ACCESS_READ;
}

void invalidate_range(ThreadExecutionState &runtime, std::uint64_t address,
	std::uint64_t size) noexcept
{
	if (size == 0)
		return;
	const auto bounded = static_cast<std::size_t>(std::min<std::uint64_t>(
		size, std::numeric_limits<std::size_t>::max()));
	runtime.x86_executor.invalidate_cached_code(address, bounded);
	constexpr std::uint64_t page_size = 4096;
	const auto final = address + size - 1U;
	for (auto page = address & ~(page_size - 1U);;
		page += page_size) {
		runtime.memory.invalidate_code(page);
		if (page >= (final & ~(page_size - 1U)) ||
			page > std::numeric_limits<std::uint64_t>::max() - page_size)
			break;
	}
}

} // namespace

WineCpuHostcallResult handle_wine_cpu_hostcall(arm64::CpuState &arm64_cpu,
	ThreadExecutionState &runtime, unsigned long generation,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt) noexcept
{
	std::array<std::byte, 4> instruction{};
	if (!runtime.memory.fetch(arm64_cpu.pc, instruction) ||
		load_u32(instruction) != UML_WINE_CPU_BRK_INSTRUCTION)
		return {};

	WineCpuHostcallResult hostcall{
		.disposition = WineCpuHostcallDisposition::resume};
	const auto request_address = arm64_cpu.registers[0];
	uml_wine_cpu_request request{};
	if (!read_request(runtime.memory, request_address, request)) {
		hostcall.disposition = WineCpuHostcallDisposition::memory_fault;
		hostcall.fault = {request_address, MemoryAccess::read};
		return hostcall;
	}

	const bool retrying_linux_fault =
		(request.flags & UML_WINE_CPU_FLAG_LINUX_FAULT_RETRY) != 0;
	const auto retried_fault_address = request.fault_address;
	const auto retried_fault_access = request.fault_access;
	const auto retried_fault_rip = request.context.rip;
	request.status = UML_WINE_CPU_STATUS_BAD_REQUEST;
	request.stop_reason = UML_WINE_CPU_STOP_NONE;
	request.capabilities = capabilities;
	request.fault_address = 0;
	request.retired_instructions = 0;
	request.fault_access = UML_WINE_CPU_ACCESS_READ;
	request.syndrome = 0;
	const bool header_valid =
		request.magic == UML_WINE_CPU_PROTOCOL_MAGIC &&
		request.version == UML_WINE_CPU_PROTOCOL_VERSION &&
		request.size == sizeof(request);
	if (header_valid) {
		switch (request.operation) {
		case UML_WINE_CPU_OPERATION_QUERY:
			request.status = UML_WINE_CPU_STATUS_OK;
			break;
		case UML_WINE_CPU_OPERATION_RESET:
			runtime.x86_cpu = {};
			runtime.x86_executor = x86_64::gadgets::Executor{};
			request.status = UML_WINE_CPU_STATUS_OK;
			break;
		case UML_WINE_CPU_OPERATION_INVALIDATE:
			if (request.invalidate_size != 0 &&
				request.invalidate_address >
					std::numeric_limits<std::uint64_t>::max() -
					(request.invalidate_size - 1U)) {
				request.status = UML_WINE_CPU_STATUS_BAD_REQUEST;
				break;
			}
			invalidate_range(runtime, request.invalidate_address,
				request.invalidate_size);
			request.status = UML_WINE_CPU_STATUS_OK;
			break;
		case UML_WINE_CPU_OPERATION_RUN: {
			if (request.context.execution_mode > 1U) {
				request.flags &= ~UML_WINE_CPU_FLAG_LINUX_FAULT_RETRY;
				request.status = UML_WINE_CPU_STATUS_UNSUPPORTED;
				break;
			}
			load_context(runtime.x86_cpu, request.context);
			/* arm64 translations of the same process live in caches this
			 * executor's registry does not see. */
			runtime.x86_executor.publish_every_executable_store(true);
			EcBitmap bitmap{request.ec_bitmap};
			const bool bitmap_valid =
				(request.flags & UML_WINE_CPU_FLAG_EC_BITMAP_VALID) != 0;
			const auto run = run_x86_gadgets(runtime.x86_cpu,
				runtime.memory, runtime.x86_executor, generation,
				instruction_budget, interrupt,
				bitmap_valid ? is_native_target : nullptr,
				bitmap_valid ? &bitmap : nullptr);
			save_context(runtime.x86_cpu, request.context);
			request.stop_reason = stop_reason_for(run);
			request.fault_address = run.execution.fault.address;
			request.fault_access = access_for(run.execution.fault.access);
			request.syndrome = run.execution.syndrome;
			request.retired_instructions = run.execution.retired_instructions;
			hostcall.retired_instructions =
				run.execution.retired_instructions;
			request.status = UML_WINE_CPU_STATUS_OK;
			const bool repeated_linux_fault = retrying_linux_fault &&
				request.stop_reason == UML_WINE_CPU_STOP_MEMORY_FAULT &&
				run.execution.retired_instructions == 0 &&
				request.context.rip == retried_fault_rip &&
				request.fault_address == retried_fault_address &&
				request.fault_access == retried_fault_access;
			if (request.stop_reason == UML_WINE_CPU_STOP_MEMORY_FAULT &&
				!repeated_linux_fault) {
				request.flags |= UML_WINE_CPU_FLAG_LINUX_FAULT_RETRY;
				hostcall.disposition =
					WineCpuHostcallDisposition::memory_fault;
				hostcall.fault = run.execution.fault;
			} else {
				request.flags &= ~UML_WINE_CPU_FLAG_LINUX_FAULT_RETRY;
			}
			break;
		}
		default:
			request.status = UML_WINE_CPU_STATUS_UNSUPPORTED;
			break;
		}
	}

	if (!write_request(runtime.memory, request_address, request)) {
		hostcall.disposition = WineCpuHostcallDisposition::memory_fault;
		hostcall.fault = {request_address, MemoryAccess::write};
		return hostcall;
	}
	if (hostcall.disposition == WineCpuHostcallDisposition::resume)
		arm64_cpu.pc += sizeof(std::uint32_t);
	return hostcall;
}

} // namespace uml::runtime

#endif
