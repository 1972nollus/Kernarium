// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "address_space_types.hpp"

#include <cstdint>
#include <memory>

namespace uml::memory {

[[nodiscard]] bool execution_reader_begin(
	GenerationState &state, std::uint64_t generation,
	std::uint64_t guest_pc);
void execution_reader_end(GenerationState &state);

class ExecutionQuiesce final {
public:
	ExecutionQuiesce(std::shared_ptr<GenerationState> state,
		std::uint64_t generation);
	ExecutionQuiesce(const ExecutionQuiesce &) = delete;
	ExecutionQuiesce &operator=(const ExecutionQuiesce &) = delete;
	~ExecutionQuiesce();

private:
	std::shared_ptr<GenerationState> state_;
	std::uint64_t generation_;
};

} // namespace uml::memory
