// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "address_space_types.hpp"

#include <cstdint>
#include <cstddef>
#include <memory>

#ifndef UML_RUNTIME_PROFILING
#define UML_RUNTIME_PROFILING 0
#endif

#ifndef UML_MEMORY_PROFILE_TESTING
#define UML_MEMORY_PROFILE_TESTING 0
#endif

namespace uml::memory {

[[nodiscard]] bool execution_reader_begin(
	GenerationState &state, std::uint64_t generation,
	std::uint64_t guest_pc) noexcept;
void execution_reader_end(GenerationState &state) noexcept;

class ExecutionQuiesce final {
public:
	ExecutionQuiesce(std::shared_ptr<GenerationState> state,
		std::uint64_t generation);
	ExecutionQuiesce(const ExecutionQuiesce &) = delete;
	ExecutionQuiesce &operator=(const ExecutionQuiesce &) = delete;
	~ExecutionQuiesce();

private:
	std::shared_ptr<GenerationState> state_;
#if UML_RUNTIME_PROFILING
	std::uint64_t generation_;
#endif
};

#if UML_RUNTIME_PROFILING && UML_MEMORY_PROFILE_TESTING
namespace testing {
[[nodiscard]] std::size_t execution_gate_profile_count() noexcept;
}
#endif

} // namespace uml::memory
