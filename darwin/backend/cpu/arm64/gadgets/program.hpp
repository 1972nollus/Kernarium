// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../cpu_state.hpp"
#include "tlb.hpp"

#include <cstddef>
#include <cstdint>
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
};

class Program final {
public:
	Program(std::uint64_t guest_address, std::size_t instruction_count,
		std::vector<std::uintptr_t> words) noexcept;

	[[nodiscard]] std::uint64_t guest_address() const noexcept;
	[[nodiscard]] std::size_t instruction_count() const noexcept;
	[[nodiscard]] std::span<const std::uintptr_t> words() const noexcept;

private:
	std::uint64_t guest_address_{};
	std::size_t instruction_count_{};
	std::vector<std::uintptr_t> words_;
};

struct BuildResult final {
	Program program;
	bool complete{};
	std::uint64_t stop_address{};
	std::uint32_t stop_instruction{};
};

class Builder final {
public:
	[[nodiscard]] BuildResult build(
		std::uint64_t guest_address,
		std::span<const std::uint32_t> instructions) const;
};

[[nodiscard]] Event execute(const Program &program, CpuState &cpu,
			    Tlb &tlb) noexcept;

} // namespace uml::backend::arm64::gadgets
