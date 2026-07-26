// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../cpu_state.hpp"
#include "tlb.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <span>
#include <vector>

namespace uml::backend::arm64::gadgets {

enum class Event : int {
	fatal = -1,
	continued = 0,
	system_call = 1,
	memory_fault = 2,
	interrupt = 3,
	illegal_instruction = 4,
	breakpoint = 5,
};

class Program final {
public:
	Program(std::uint64_t guest_address, std::size_t instruction_count,
		std::pmr::vector<std::uintptr_t> words,
		bool accesses_fp_state) noexcept;

	[[nodiscard]] std::uint64_t guest_address() const noexcept
	{
		return guest_address_;
	}
	[[nodiscard]] std::size_t instruction_count() const noexcept
	{
		return instruction_count_;
	}
	[[nodiscard]] std::span<const std::uintptr_t> words() const noexcept
	{
		return words_;
	}
	[[nodiscard]] bool accesses_fp_state() const noexcept
	{
		return accesses_fp_state_;
	}

private:
	std::uint64_t guest_address_{};
	std::size_t instruction_count_{};
	std::pmr::vector<std::uintptr_t> words_;
	bool accesses_fp_state_{};
};

struct BuildResult final {
	Program program;
	bool complete{};
	std::uint64_t stop_address{};
	std::uint32_t stop_instruction{};
};

/* Direct control-flow gadgets load these immutable-layout cells through the
 * data stream. The cache may retarget a cell from the shared exit stream to a
 * translated successor without modifying executable memory. */
struct BlockLink final {
	const std::uintptr_t *stream{};
	std::size_t instruction_count{};
	std::uint64_t guest_pc{};
};
static_assert(offsetof(BlockLink, stream) == 0);
static_assert(offsetof(BlockLink, instruction_count) == 8);
static_assert(offsetof(BlockLink, guest_pc) == 16);
static_assert(sizeof(BlockLink) == 24);

struct IndirectLinkEntry final {
	std::uint64_t guest_pc{};
	const BlockLink *link{};
};
static_assert(offsetof(IndirectLinkEntry, guest_pc) == 0);
static_assert(offsetof(IndirectLinkEntry, link) == 8);
static_assert(sizeof(IndirectLinkEntry) == 16);

/* Two target cells cover the monomorphic and common bimorphic branch/return
 * sites while keeping the assembly hit path compact. ProgramCache owns and
 * trains these data-only cells; translated streams only retain stable
 * pointers to them. */
struct IndirectLink final {
	std::array<IndirectLinkEntry, 2> entries{};
	std::uint8_t next_replacement{};
};
static_assert(offsetof(IndirectLink, entries) == 0);
static_assert(offsetof(IndirectLink, next_replacement) == 32);
static_assert(sizeof(IndirectLink) == 40);

class Builder final {
public:
	using LinkResolver =
		std::function<const BlockLink *(std::uint64_t guest_pc)>;
	using IndirectLinkResolver =
		std::function<IndirectLink *(std::uint64_t source_pc)>;

	[[nodiscard]] BuildResult build(
		std::uint64_t guest_address,
		std::span<const std::uint32_t> instructions,
		LinkResolver resolve_link = {},
		std::pmr::memory_resource *storage =
			std::pmr::get_default_resource(),
		IndirectLinkResolver resolve_indirect_link = {}) const;
};

[[nodiscard]] Event execute(const Program &program, CpuState &cpu,
			    Tlb &tlb) noexcept;
[[nodiscard]] Event execute(const Program &program, CpuState &cpu,
			    Tlb &tlb, std::size_t &executed) noexcept;
[[nodiscard]] Event execute_preserving_native_fp_environment(
	const Program &program, CpuState &cpu, Tlb &tlb,
	std::size_t &executed) noexcept;
void restore_native_fp_environment(CpuState &cpu) noexcept;

} // namespace uml::backend::arm64::gadgets
