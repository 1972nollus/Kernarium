// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/decoder.hpp"
#include "memory/guest_address_space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uml::cpu::x86_64::gadgets {

struct Gadget;
class Executor;

/* A gadget is immutable normalized instruction data. Translation resolves it
 * exclusively to precompiled arm64 assembly entries; no guest instruction is
 * copied into executable memory and no semantic C++ executor remains.
 */
struct Gadget final {
	std::uint64_t address{};
	Instruction instruction{};
};

/* Cached direct branches name stable, data-only link records. A reset link
 * enters its private fallback stream; an active link enters the cached target
 * without modifying executable memory. The target bounds preserve x86's
 * self-modifying-code check while a native chain crosses program objects.
 */
struct BlockLink final {
	const std::uintptr_t *stream{};
	std::size_t instruction_count{};
	std::uint64_t guest_rip{};
	std::uint64_t program_begin{};
	std::uint64_t program_end{};
};
static_assert(offsetof(BlockLink, stream) == 0);
static_assert(offsetof(BlockLink, instruction_count) == 8);
static_assert(offsetof(BlockLink, guest_rip) == 16);
static_assert(offsetof(BlockLink, program_begin) == 24);
static_assert(offsetof(BlockLink, program_end) == 32);
static_assert(sizeof(BlockLink) == 40);

struct IndirectLinkEntry final {
	std::uint64_t guest_rip{};
	const BlockLink *link{};
};
static_assert(offsetof(IndirectLinkEntry, guest_rip) == 0);
static_assert(offsetof(IndirectLinkEntry, link) == 8);
static_assert(sizeof(IndirectLinkEntry) == 16);

/* Match arm64's monomorphic/bimorphic indirect tier. The translated hit path
 * reads only the two target/link pairs; replacement and live-list state stay
 * in data and never require executable publication. */
struct IndirectLink final {
	std::array<IndirectLinkEntry, 2> entries{};
	std::uint8_t next_replacement{};
	bool listed{};
};
static_assert(offsetof(IndirectLink, entries) == 0);
static_assert(offsetof(IndirectLink, next_replacement) == 32);
static_assert(offsetof(IndirectLink, listed) == 33);
static_assert(sizeof(IndirectLink) == 40);

struct ProgramLinkResolver final {
	void *context{};
	const BlockLink *(*resolve)(void *, std::uint64_t, ExecutionMode){};
	IndirectLink *(*resolve_indirect)(
		void *, std::uint64_t, ExecutionMode){};
};

class Program final {
public:
	Program() = default;
	Program(ExecutionMode mode, std::uint64_t address, std::size_t byte_count,
		std::vector<Gadget> gadgets, ProgramLinkResolver links = {});

	[[nodiscard]] ExecutionMode execution_mode() const noexcept { return mode_; }
	[[nodiscard]] std::uint64_t address() const noexcept { return address_; }
	[[nodiscard]] std::size_t byte_count() const noexcept { return byte_count_; }
	[[nodiscard]] std::span<const Gadget> stream() const noexcept
	{
		return gadgets_;
	}
	[[nodiscard]] std::span<const std::uintptr_t> native_stream() const noexcept
	{
		return native_stream_;
	}
	[[nodiscard]] std::size_t instruction_count() const noexcept
	{
		return gadgets_.size();
	}

private:
	ExecutionMode mode_{ExecutionMode::long64};
	std::uint64_t address_{};
	std::size_t byte_count_{};
	std::vector<Gadget> gadgets_;
	std::vector<std::uintptr_t> native_stream_;
};

enum class BuildStatus : std::uint8_t {
	built,
	fetch_fault,
	illegal_instruction,
	allocation_failure,
};

struct BuildResult final {
	BuildStatus status{BuildStatus::illegal_instruction};
	Program program{};
	std::uint64_t fault_address{};
};

[[nodiscard]] BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, std::size_t maximum_instructions = 64) noexcept;
[[nodiscard]] BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions = 64) noexcept;
[[nodiscard]] BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions, ProgramLinkResolver links) noexcept;

struct ExecuteResult final {
	::uml::cpu::EngineEvent event{::uml::cpu::EngineEvent::fatal};
	std::size_t retired_instructions{};
	::uml::cpu::FaultInfo fault{};
	bool instruction_budget_exhausted{};
};

[[nodiscard]] ExecuteResult execute(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt = {}) noexcept;
[[nodiscard]] ExecuteResult execute(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt = {}) noexcept;

} // namespace uml::cpu::x86_64::gadgets
