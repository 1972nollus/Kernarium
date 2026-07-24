// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../cpu_state.hpp"
#include "tlb.hpp"

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

class Builder final {
public:
	using LinkResolver =
		std::function<const BlockLink *(std::uint64_t guest_pc)>;

	[[nodiscard]] BuildResult build(
		std::uint64_t guest_address,
		std::span<const std::uint32_t> instructions,
		LinkResolver resolve_link = {},
		std::pmr::memory_resource *storage =
			std::pmr::get_default_resource()) const;
};

[[nodiscard]] Event execute(const Program &program, CpuState &cpu,
			    Tlb &tlb) noexcept;
[[nodiscard]] Event execute(const Program &program, CpuState &cpu,
			    Tlb &tlb, std::size_t &executed) noexcept;

} // namespace uml::backend::arm64::gadgets
