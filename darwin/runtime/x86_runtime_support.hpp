// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* What every x86-64 engine's runtime shares: the static MMU execution scope
 * a dispatch runs in, and the mapping of a non-canonical access to the
 * exception x86 raises for it. */

#if DARWIN_UML_X86_64_GUEST

#include "cpu/x86_64/cpu_state.hpp"
#include "memory/static_guest_memory.hpp"
#include "uml_darwin_static_mmu.h"

#include <cstdint>

namespace uml::runtime {

/* x86 exception vectors the runtime reports as a breakpoint's syndrome. */
inline constexpr std::uint32_t x86_stack_segment_fault = 12;
inline constexpr std::uint32_t x86_general_protection = 13;

class X86StaticExecutionScope final {
public:
	X86StaticExecutionScope(unsigned long generation, std::uint64_t guest_pc)
		: context_{darwin_uml_static_vm_execution_context_create(generation)}
	{
		entered_ = context_ != nullptr &&
			darwin_uml_static_vm_execution_context_begin(
				context_, guest_pc) == 0;
	}

	~X86StaticExecutionScope()
	{
		if (entered_)
			(void)darwin_uml_static_vm_execution_context_end(context_);
		darwin_uml_static_vm_execution_context_destroy(context_);
	}

	X86StaticExecutionScope(const X86StaticExecutionScope &) = delete;
	X86StaticExecutionScope &operator=(const X86StaticExecutionScope &) = delete;

	[[nodiscard]] bool entered() const noexcept { return entered_; }
	[[nodiscard]] void *context() const noexcept { return context_; }
	[[nodiscard]] darwin_uml_static_sequences sequences() const noexcept
	{
		return darwin_uml_static_vm_execution_context_sequences(context_);
	}

private:
	void *context_{};
	bool entered_{};
};

/* Bits 63 to 47 all equal: an address of the 4-level paging x86-64 Linux
 * runs userspace under. */
[[nodiscard]] constexpr bool x86_canonical(std::uint64_t address) noexcept
{
	const auto high = static_cast<std::int64_t>(address) >> 47;
	return high == 0 || high == -1;
}

/* A non-canonical address raises #SS when the stack segment forms it --
 * an implicit stack access, or an operand based on RSP or RBP with no
 * segment override -- and #GP otherwise, neither with an error code. */
[[nodiscard]] std::uint32_t x86_noncanonical_vector(
	::uml::memory::StaticGuestMemory &memory,
	const ::uml::cpu::x86_64::CpuState &cpu) noexcept;

/* Called for each x86 dispatch whose mm is shared. Setting
 * UML_X86_MM_SHARED_LOG to a writable path makes the runtime append one line
 * the first time each address space (generation) dispatches that way, with
 * the RIP it resumed at; tests/integration/x86_64/run_guest_x86_64_mm_shared.py
 * reads it. It is a file rather than stderr because the emulator's stderr
 * shares the guest console. Nothing is opened, locked or recorded when the
 * variable is unset. */
void note_x86_mm_shared(unsigned long generation, std::uint64_t rip) noexcept;

} // namespace uml::runtime

#endif
