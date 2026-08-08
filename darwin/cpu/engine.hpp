/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/host_capabilities.hpp"
#include "memory/guest_address_space.hpp"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>

namespace uml::memory {
class StaticGuestMemory;
}

namespace uml::cpu {

using GuestAddressSpace = ::uml::memory::GuestAddressSpace;

enum class EngineKind {
	interpreter,
	gadgets,
	jit,
};

/* Stable event ABI shared by all engines. Engine-specific translation and
 * execution headers must not be required to exchange a boundary result. */
enum class EngineEvent : int {
	fatal = -1,
	continued = 0,
	system_call = 1,
	memory_fault = 2,
	interrupt = 3,
	illegal_instruction = 4,
	breakpoint = 5,
	gcs_fault = 6,
	floating_point_trap = 7,
	pc_alignment = 8,
};

enum class ExitReason {
	system_call,
	memory_fault,
	interrupt,
	illegal_instruction,
	breakpoint,
	gcs_fault,
	floating_point_trap,
	pc_alignment,
	instruction_budget,
	fatal,
};

using InterruptPoll = bool (*)(void *) noexcept;

struct FaultInfo final {
	std::uint64_t address{};
	::uml::memory::MemoryAccess access{::uml::memory::MemoryAccess::read};
};

struct InterruptToken final {
	const unsigned int *sequence{};
	unsigned int snapshot{};
	const std::atomic_bool *stop{};
	InterruptPoll poll{};
	void *poll_context{};

	[[nodiscard]] bool state_pending() const noexcept
	{
		return (sequence != nullptr &&
			__atomic_load_n(sequence, __ATOMIC_ACQUIRE) != snapshot) ||
			(stop != nullptr && stop->load(std::memory_order_acquire));
	}

	[[nodiscard]] bool callback_pending() const noexcept
	{
		return poll != nullptr && poll(poll_context);
	}

	[[nodiscard]] bool pending() const noexcept
	{
		return state_pending() || callback_pending();
	}
};

using BoundaryHook = bool (*)(arm64::CpuState &, void *) noexcept;

struct ExecutionRequest final {
	arm64::CpuState &cpu;
	GuestAddressSpace &memory;
	/* The runtime supplies this typed view whenever memory is backed by the
	 * static MMU. Translated engines require it for their fast path, while the
	 * interpreter uses its precise compound-access fault metadata. Keeping the
	 * view explicit removes RTTI from every guest entry. */
	::uml::memory::StaticGuestMemory *translated_memory{};
	unsigned long generation{};
	std::size_t instruction_budget{};
	InterruptToken interrupt{};
	BoundaryHook boundary{};
	void *boundary_context{};
};

struct ExecutionResult final {
	ExitReason reason{ExitReason::fatal};
	std::size_t retired_instructions{};
	FaultInfo fault{};
	std::uint32_t syndrome{};
};

struct EngineConfiguration final {
	bool enable_guarded_control_flow{};
	bool enable_pointer_authentication{true};
	HostCapabilities host_capabilities{::uml::cpu::host_capabilities()};
};

class CpuEngine {
public:
	virtual ~CpuEngine() = default;
	[[nodiscard]] virtual EngineKind kind() const noexcept = 0;
	/* Guest exits, including an internal host-resource failure, are returned as
	 * ExecutionResult. This boundary is noexcept because it is called from the
	 * fixed-arity C kernel bridge; concrete engines must not unwind through it.
	 * Before returning, an engine must also commit any guest state held in host
	 * architectural registers and release its engine-private execution state.
	 * Callers must never need an engine-specific cleanup operation after run().
	 */
	[[nodiscard]] virtual ExecutionResult run(ExecutionRequest &) noexcept = 0;
};

} // namespace uml::cpu
