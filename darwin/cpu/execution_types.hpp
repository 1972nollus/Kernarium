// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "memory/guest_address_space.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace uml::cpu {

/* ISA-neutral exits shared by every guest CPU backend. */
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
	overflow = 9,
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

struct ExecutionResult final {
	ExitReason reason{ExitReason::fatal};
	std::size_t retired_instructions{};
	FaultInfo fault{};
	std::uint32_t syndrome{};
};

} // namespace uml::cpu
