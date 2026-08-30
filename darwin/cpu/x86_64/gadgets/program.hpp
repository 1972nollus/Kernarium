// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/decoder.hpp"
#include "memory/guest_address_space.hpp"

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

class Program final {
public:
	Program() = default;
	Program(std::uint64_t address, std::size_t byte_count,
		std::vector<Gadget> gadgets);

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
