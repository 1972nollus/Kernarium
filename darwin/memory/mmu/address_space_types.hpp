// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "mapped_backing.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace uml::memory {

struct Mapping final {
	std::shared_ptr<MappedBacking> backing;
	std::uint64_t offset;
	int fd;
	int protection;
	bool guest_code;
	bool guest_writable;
};

struct GenerationState final {
	std::mutex walker_mutex;
	mutable std::shared_mutex mappings_mutex;
	std::unordered_map<std::uint64_t, Mapping> mappings;
	std::atomic_uint64_t mapping_changes{1};
	std::atomic_uint64_t code_changes{1};
	std::atomic_uint64_t invalidation_epoch{};
	std::atomic_uint64_t execution_interrupts{};
	std::atomic_uint64_t execution_gate{};
	struct MappingChange final {
		std::uint64_t sequence;
		std::uint64_t address;
		std::uint64_t length;
	};
	std::deque<MappingChange> mapping_history;
};

struct MappingOperation final {
	bool map;
	std::uint64_t address;
	std::uint64_t length;
	Mapping mapping;
};

struct MappingBatch final {
	unsigned depth{};
	std::unordered_map<int, std::shared_ptr<MappedBacking>> backings;
	std::vector<MappingOperation> operations;
	std::uint64_t generation{};
	std::uint64_t invalidation_epoch{};
};

struct ActiveWalker final {
	std::uint64_t generation{};
	std::shared_ptr<GenerationState> state;
};

struct ActiveGeneration final {
	std::uint64_t generation{};
	std::shared_ptr<GenerationState> state;
};

struct GenerationReference final {
	std::shared_ptr<GenerationState> owner;
	GenerationState *state{};
};

struct ActiveExecution final {
	std::uint64_t generation{};
	GenerationState *state{};
	std::shared_ptr<GenerationState> owner;
};

struct ExecutionContext final {
	std::shared_ptr<GenerationState> state;
	std::uint64_t generation{};
	std::uint64_t execution_interrupts{};
	bool entered{};
};

} // namespace uml::memory
