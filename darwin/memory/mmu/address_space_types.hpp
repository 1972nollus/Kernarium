// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "mapped_backing.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace uml::memory {

/* std::hash of an integer is the identity, and libc++ keeps a power-of-two
 * bucket count whenever one is requested through reserve(), constraining a
 * hash by masking rather than by a modulus.  A page address carries
 * log2(page_size) low zero bits, so an identity hash under a power-of-two mask
 * reaches only bucket_count / page_size distinct buckets: at 16 KiB pages a
 * 65,536-bucket table uses four of them, every lookup walks a chain thousands
 * of nodes long, and filling the table becomes quadratic.  Fold the page
 * number so the distribution no longer depends on the bucket count the
 * container happens to choose.
 */
struct GuestPageHash final {
	[[nodiscard]] std::size_t operator()(
		const std::uint64_t address) const noexcept
	{
		auto value = address / DARWIN_UML_GUEST_PAGE_SIZE;
		value ^= value >> 33;
		value *= 0xff51'afd7'ed55'8ccdULL;
		value ^= value >> 29;
		return static_cast<std::size_t>(value);
	}
};

struct Mapping final {
	std::shared_ptr<MappedBacking> backing;
	std::uint64_t offset;
	int fd;
	int protection;
	bool guest_code;
	bool guest_writable;
};

struct AddressSpaceLifetime final {
	std::atomic_bool retired{};
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
	std::atomic_bool retired{};
	std::shared_ptr<AddressSpaceLifetime> address_space_lifetime;
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
	const void *address_space{};
	unsigned depth{};
	std::unordered_map<int, std::shared_ptr<MappedBacking>> backings;
	std::vector<MappingOperation> operations;
	std::uint64_t generation{};
	std::uint64_t invalidation_epoch{};
	std::shared_ptr<GenerationState> state;
};

struct ActiveWalker final {
	const void *address_space{};
	std::uint64_t generation{};
	std::shared_ptr<GenerationState> state;
};

struct ActiveGeneration final {
	const void *address_space{};
	std::uint64_t generation{};
	std::shared_ptr<GenerationState> state;
};

struct GenerationReference final {
	std::shared_ptr<GenerationState> owner;
	GenerationState *state{};
};

struct ActiveExecution final {
	const void *address_space{};
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
