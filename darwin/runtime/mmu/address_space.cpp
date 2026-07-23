// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_static_mmu.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <pthread.h>
#include <shared_mutex>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <unistd.h>

namespace uml::runtime {
namespace {

constexpr std::uint64_t page_size = 4096;
constexpr std::uint64_t address_limit = 0x6000'0000'0000ULL;

struct BackingKey final {
	std::uint64_t device;
	std::uint64_t inode;

	bool operator==(const BackingKey &) const = default;
};

struct BackingKeyHash final {
	[[nodiscard]] std::size_t operator()(const BackingKey &key) const noexcept
	{
		auto value = key.inode ^ (key.device * 0x9e37'79b9'7f4a'7c15ULL);
		value ^= value >> 33;
		return static_cast<std::size_t>(value);
	}
};

/* One non-executable shared mapping covers the complete UML physical-memory
 * file.  Besides avoiding a host mapping per 4 KiB guest page, this is the
 * iSH-style MMU invariant that lets a TLB hit become pointer arithmetic.
 */
class MappedBacking final {
public:
	static std::shared_ptr<MappedBacking> create(int fd, const struct stat &status)
	{
		if (status.st_size <= 0 ||
		    static_cast<std::uintmax_t>(status.st_size) > SIZE_MAX)
			return {};
		const auto size = static_cast<std::size_t>(status.st_size);
		void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE,
				     MAP_SHARED, fd, 0);
		if (mapping == MAP_FAILED)
			return {};
		return std::shared_ptr<MappedBacking>{
			new MappedBacking{static_cast<std::byte *>(mapping), size}};
	}

	MappedBacking(const MappedBacking &) = delete;
	MappedBacking &operator=(const MappedBacking &) = delete;

	~MappedBacking()
	{
		if (data_ != nullptr)
			(void)munmap(data_, size_);
	}

	[[nodiscard]] std::byte *at(std::uint64_t offset) const noexcept
	{
		if (offset >= size_)
			return nullptr;
		return data_ + static_cast<std::size_t>(offset);
	}

	[[nodiscard]] std::size_t size() const noexcept { return size_; }

	[[nodiscard]] std::uint8_t load_allocation_tag(
		std::uint64_t offset) const noexcept
	{
		const auto page = offset / page_size;
		const auto index = static_cast<std::size_t>(
			(offset & (page_size - 1)) / 16);
		std::shared_lock lock{tags_mutex_};
		const auto found = tag_pages_.find(page);
		return found == tag_pages_.end() ? 0 :
			found->second->tags[index].load(std::memory_order_acquire);
	}

	[[nodiscard]] bool store_allocation_tag(
		std::uint64_t offset, std::uint8_t tag) noexcept
	{
		const auto page = offset / page_size;
		const auto index = static_cast<std::size_t>(
			(offset & (page_size - 1)) / 16);
		TagPage *tags = nullptr;
		{
			std::shared_lock lock{tags_mutex_};
			const auto found = tag_pages_.find(page);
			if (found != tag_pages_.end())
				tags = found->second.get();
		}
		if (tags == nullptr) {
			std::lock_guard lock{tags_mutex_};
			auto &slot = tag_pages_[page];
			if (!slot) {
				try {
					slot = std::make_unique<TagPage>();
				} catch (...) {
					return false;
				}
			}
			tags = slot.get();
		}
		tags->tags[index].store(tag & 15U, std::memory_order_release);
		return true;
	}

private:
	struct TagPage final {
		TagPage() noexcept
		{
			for (auto &tag : tags)
				tag.store(0, std::memory_order_relaxed);
		}

		std::array<std::atomic_uint8_t, page_size / 16> tags;
	};

	MappedBacking(std::byte *data, std::size_t size) : data_(data), size_(size) {}

	std::byte *data_;
	std::size_t size_;
	mutable std::shared_mutex tags_mutex_;
	std::unordered_map<std::uint64_t, std::unique_ptr<TagPage>> tag_pages_;
};

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

constexpr std::uint64_t execution_gate_closed = std::uint64_t{1} << 63;
constexpr std::uint64_t execution_reader_mask = ~execution_gate_closed;

[[nodiscard]] bool gate_profile_enabled() noexcept
{
	static const bool enabled = [] {
		const auto *value = std::getenv("UML_PROFILE_TLB_GATE");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return enabled;
}

[[nodiscard]] std::uint64_t current_thread_id() noexcept
{
	std::uint64_t identifier = 0;
	(void)pthread_threadid_np(nullptr, &identifier);
	return identifier;
}

struct ReaderProfile final {
	std::atomic_uint64_t generation{};
	std::atomic_uint64_t guest_pc{};
	std::uint64_t thread{};
};

std::mutex &reader_profiles_mutex()
{
	static std::mutex mutex;
	return mutex;
}

std::vector<std::unique_ptr<ReaderProfile>> &reader_profiles()
{
	static std::vector<std::unique_ptr<ReaderProfile>> profiles;
	return profiles;
}

ReaderProfile &local_reader_profile()
{
	static thread_local ReaderProfile *profile = [] {
		auto owned = std::make_unique<ReaderProfile>();
		owned->thread = current_thread_id();
		auto *result = owned.get();
		std::lock_guard lock{reader_profiles_mutex()};
		reader_profiles().push_back(std::move(owned));
		return result;
	}();
	return *profile;
}

void profile_reader_begin(std::uint64_t generation, std::uint64_t guest_pc)
{
	if (!gate_profile_enabled())
		return;
	auto &profile = local_reader_profile();
	profile.guest_pc.store(guest_pc, std::memory_order_relaxed);
	profile.generation.store(generation, std::memory_order_release);
}

void profile_reader_end()
{
	if (!gate_profile_enabled())
		return;
	auto &profile = local_reader_profile();
	profile.generation.store(0, std::memory_order_release);
	profile.guest_pc.store(0, std::memory_order_relaxed);
}

void profile_stalled_readers(std::uint64_t generation)
{
	const auto *path = std::getenv("UML_PROFILE_TLB_GATE");
	std::FILE *output = stderr;
	if (path != nullptr && path[0] == '/') {
		if (auto *file = std::fopen(path, "a"); file != nullptr)
			output = file;
	}
	std::fprintf(output,
		"Darwin static MMU: writer thread=%llu generation=%llu waiting for execution reader(s):",
		static_cast<unsigned long long>(current_thread_id()),
		static_cast<unsigned long long>(generation));
	{
		std::lock_guard lock{reader_profiles_mutex()};
		for (const auto &profile : reader_profiles()) {
			if (profile->generation.load(std::memory_order_acquire) !=
			    generation)
				continue;
			std::fprintf(output, " thread=%llu guest-pc=%#llx",
				static_cast<unsigned long long>(profile->thread),
				static_cast<unsigned long long>(profile->guest_pc.load(
					std::memory_order_acquire)));
		}
	}
	std::fputc('\n', output);
	std::fflush(output);
	if (output != stderr)
		std::fclose(output);
}

bool execution_reader_begin(GenerationState &state, std::uint64_t generation,
			    std::uint64_t guest_pc)
{
	auto value = state.execution_gate.load(std::memory_order_acquire);
	for (;;) {
		while ((value & execution_gate_closed) != 0) {
			state.execution_gate.wait(value, std::memory_order_relaxed);
			value = state.execution_gate.load(std::memory_order_acquire);
		}
		if ((value & execution_reader_mask) == execution_reader_mask)
			return false;
		if (state.execution_gate.compare_exchange_weak(
				value, value + 1, std::memory_order_acquire,
				std::memory_order_relaxed)) {
			profile_reader_begin(generation, guest_pc);
			return true;
		}
	}
}

void execution_reader_end(GenerationState &state)
{
	profile_reader_end();
	const auto previous = state.execution_gate.fetch_sub(
		1, std::memory_order_release);
	/* A zero-reader transition only needs a wakeup after a writer has closed
	 * the gate.  Calling atomic::notify_all for every translated block was a
	 * measurable SMP hot path even when no TLB shootdown was pending. */
	if ((previous & execution_gate_closed) != 0 &&
	    (previous & execution_reader_mask) == 1)
		state.execution_gate.notify_all();
}

class ExecutionQuiesce final {
public:
	ExecutionQuiesce(std::shared_ptr<GenerationState> state,
		std::uint64_t generation)
		: state_{std::move(state)}, generation_{generation}
	{
		auto value = state_->execution_gate.load(std::memory_order_acquire);
		for (;;) {
			if ((value & execution_gate_closed) != 0) {
				state_->execution_gate.wait(
					value, std::memory_order_relaxed);
				value = state_->execution_gate.load(
					std::memory_order_acquire);
				continue;
			}
			if (state_->execution_gate.compare_exchange_weak(
					value, value | execution_gate_closed,
					std::memory_order_acq_rel,
					std::memory_order_relaxed))
				break;
		}
		/* A translated userspace loop may otherwise retain an execution
		 * reader until its full dispatch budget expires.  Request a prompt
		 * return at the next (at most 64-instruction) translated-block
		 * boundary before waiting for readers.  Mapping mutation remains
		 * strictly after the reader count reaches zero.
		 */
		state_->execution_interrupts.fetch_add(1, std::memory_order_release);
		value = state_->execution_gate.load(std::memory_order_acquire);
		const auto started = std::chrono::steady_clock::now();
		bool reported = false;
		while ((value & execution_reader_mask) != 0) {
			if (!gate_profile_enabled()) {
				state_->execution_gate.wait(
					value, std::memory_order_relaxed);
			} else {
				if (!reported &&
				    std::chrono::steady_clock::now() - started >=
					    std::chrono::milliseconds{100}) {
					profile_stalled_readers(generation_);
					reported = true;
				}
				std::this_thread::sleep_for(
					std::chrono::milliseconds{1});
			}
			value = state_->execution_gate.load(
				std::memory_order_acquire);
		}
	}

	ExecutionQuiesce(const ExecutionQuiesce &) = delete;
	ExecutionQuiesce &operator=(const ExecutionQuiesce &) = delete;

	~ExecutionQuiesce()
	{
		state_->execution_gate.store(0, std::memory_order_release);
		state_->execution_gate.notify_all();
	}

private:
	std::shared_ptr<GenerationState> state_;
	std::uint64_t generation_;
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

MappingBatch &mapping_batch()
{
	static thread_local MappingBatch batch;
	return batch;
}

struct ActiveWalker final {
	std::uint64_t generation{};
	std::shared_ptr<GenerationState> state;
};

ActiveWalker &active_walker()
{
	static thread_local ActiveWalker active;
	return active;
}

constexpr std::size_t maximum_mapping_history = 4096;

struct ActiveGeneration final {
	std::uint64_t generation{};
	std::shared_ptr<GenerationState> state;
};

struct GenerationReference final {
	std::shared_ptr<GenerationState> owner;
	GenerationState *state{};
};

ActiveGeneration &active_thread_generation()
{
	static thread_local ActiveGeneration active;
	return active;
}

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

ActiveExecution &active_thread_execution()
{
	static thread_local ActiveExecution active;
	return active;
}

class AddressSpace final {
public:
	int map(std::uint64_t generation, std::uint64_t address, int fd,
		std::uint64_t offset,
		std::uint64_t length, int protection, bool guest_code,
		bool guest_writable)
	{
		if ((address | offset | length) % page_size != 0 || length == 0)
			return -1;
		auto backing = backing_for(fd);
		if (!backing || offset > backing->size() ||
		    length > backing->size() - offset)
			return -1;

		generation = generation_for(address, generation);
		auto &batch = mapping_batch();
		if (batch.depth != 0 && batch.generation == generation) {
			batch.operations.push_back({
				true, address, length,
				Mapping{std::move(backing), offset, fd, protection,
					guest_code, guest_writable}});
			return 0;
		}
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		std::lock_guard lock{state->mappings_mutex};
		map_locked(*state, address, length,
			Mapping{std::move(backing), offset, fd, protection,
				guest_code, guest_writable});
		return 0;
	}

	int protect(std::uint64_t address, std::uint64_t length, int protection)
	{
		if ((address | length) % page_size != 0 || length == 0)
			return -1;
		const auto generation = generation_for(
			address, active_thread_generation().generation);
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		std::lock_guard lock{state->mappings_mutex};
		bool changed = false;
		bool changes_code = false;
		for (std::uint64_t position = 0; position < length;
		     position += page_size) {
			auto mapping = state->mappings.find(address + position);
			if (mapping != state->mappings.end() &&
			    mapping->second.protection != protection) {
				changed = true;
				changes_code = changes_code || mapping->second.guest_code;
				mapping->second.protection = protection;
			}
		}
		if (changed)
			changed_locked(*state, changes_code, address, length);
		return 0;
	}

	int unmap(std::uint64_t generation, std::uint64_t address,
		  std::uint64_t length)
	{
		if (length == 0 || address + length < address)
			return length == 0 ? 0 : -1;
		address &= ~(page_size - 1);
		length = (length + page_size - 1) & ~(page_size - 1);
		generation = generation_for(address, generation);
		auto &batch = mapping_batch();
		if (batch.depth != 0 && batch.generation == generation) {
			batch.operations.push_back(
				{false, address, length, Mapping{}});
			return 0;
		}
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		state->invalidation_epoch.fetch_add(1, std::memory_order_release);
		std::lock_guard lock{state->mappings_mutex};
		unmap_locked(*state, address, length);
		return 0;
	}

	int activate(std::uint64_t generation)
	{
		/* A UML process is a Darwin thread, so the selected Linux mm is a
		 * thread property. A process-global selector lets a sibling switch mm
		 * between activate() and map(), installing a page in the wrong guest.
		 * Translation already carries an explicit generation; activation only
		 * supplies the implicit generation used by mapping-management calls.
		 */
		auto &active = active_thread_generation();
		if (active.generation == generation && active.state)
			return 0;
		active.generation = generation;
		active.state = state_for(generation);
		return 0;
	}

	int destroy(std::uint64_t generation)
	{
		if (generation == 0)
			return 0;
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		{
			std::lock_guard lock{state->mappings_mutex};
			state->mappings.clear();
			changed_locked(*state, true, 0, 0);
		}
		{
			std::lock_guard lock{states_mutex_};
			if (const auto found = states_.find(generation);
			    found != states_.end() && found->second == state)
				states_.erase(found);
		}
		auto &active = active_thread_generation();
		if (active.generation == generation)
			active.state.reset();
		return 0;
	}

	int matches(std::uint64_t address, int fd, std::uint64_t offset)
	{
		address &= ~(page_size - 1);
		const auto generation = generation_for(
			address, active_thread_generation().generation);
		auto reference = reference_for(generation);
		std::shared_lock lock{reference.state->mappings_mutex};
		auto iterator = reference.state->mappings.find(address);
		if (iterator == reference.state->mappings.end())
			return -1;
		return iterator->second.fd == fd && iterator->second.offset == offset;
	}

	int rebind(std::uint64_t address, int fd, std::uint64_t offset)
	{
		address &= ~(page_size - 1);
		auto backing = backing_for(fd);
		if (!backing || offset > backing->size() ||
		    page_size > backing->size() - offset)
			return -1;
		const auto generation = generation_for(
			address, active_thread_generation().generation);
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		std::lock_guard lock{state->mappings_mutex};
		auto iterator = state->mappings.find(address);
		if (iterator == state->mappings.end())
			return 0;
		if (iterator->second.fd == fd && iterator->second.offset == offset)
			return 0;
		iterator->second.backing = std::move(backing);
		iterator->second.offset = offset;
		iterator->second.fd = fd;
		changed_locked(*state, iterator->second.guest_code, address,
			page_size);
		return 0;
	}

	int has_mapping(std::uint64_t address)
	{
		address &= ~(page_size - 1);
		const auto generation = generation_for(
			address, active_thread_generation().generation);
		auto reference = reference_for(generation);
		std::shared_lock lock{reference.state->mappings_mutex};
		return reference.state->mappings.contains(address);
	}

	int copy_from_buffer(std::uint64_t address, const void *buffer,
			     std::uint64_t length)
	{
		if (length == 0)
			return 0;
		if (buffer == nullptr || address + length < address ||
		    length > page_size - (address & (page_size - 1)))
			return -1;
		const auto page = address & ~(page_size - 1);
		const auto in_page = address & (page_size - 1);
		/* The mapping must remain stable while the written range is copied,
		 * but neither the mapping table nor generation metadata is structurally
		 * changed.  Let unrelated page copies and translations proceed in
		 * parallel.  Copying only the bytes changed by uaccess is essential:
		 * a sibling guest thread may update another byte in the same 4 KiB page
		 * while the kernel operates through its Darwin 16 KiB physical shadow.
		 */
		const auto generation = generation_for(
			page, active_thread_generation().generation);
		auto reference = reference_for(generation);
		std::shared_lock lock{reference.state->mappings_mutex};
		auto iterator = reference.state->mappings.find(page);
		if (iterator == reference.state->mappings.end())
			return 0;
		auto *destination = iterator->second.backing->at(
			iterator->second.offset + in_page);
		if (destination == nullptr)
			return -1;
		std::memcpy(destination, buffer, length);
		if (iterator->second.guest_code)
			reference.state->code_changes.fetch_add(
				1, std::memory_order_release);
		return 0;
	}

	int translate(std::uint64_t generation, std::uint64_t address,
		      darwin_uml_static_access access, void **result)
	{
		if (result == nullptr)
			return DARWIN_UML_STATIC_HOST_ERROR;
		*result = nullptr;
		const auto page = address & ~(page_size - 1);
		const auto resolved_generation = generation_for(page, generation);
		/* The execution gate excludes mapping mutation in this generation.
		 * Keep each generation's mapping lookup independent so unrelated guest
		 * processes neither share a hash table nor touch the same lock cacheline.
		 */
		auto reference = reference_for(resolved_generation);
		std::shared_lock lock{reference.state->mappings_mutex};
		auto iterator = reference.state->mappings.find(page);
		if (iterator == reference.state->mappings.end())
			return DARWIN_UML_STATIC_UNMAPPED;
		const auto &mapping = iterator->second;
		const bool permitted = access == DARWIN_UML_STATIC_EXECUTE ?
			mapping.guest_code && (mapping.protection & PROT_EXEC) != 0 :
			access == DARWIN_UML_STATIC_WRITE ?
				(mapping.protection & PROT_WRITE) != 0 :
				(mapping.protection & PROT_READ) != 0;
		if (!permitted)
			return DARWIN_UML_STATIC_PERMISSION;
		auto *base = mapping.backing->at(
			mapping.offset + (address & (page_size - 1)));
		if (base == nullptr)
			return DARWIN_UML_STATIC_HOST_ERROR;
		*result = base;
		return DARWIN_UML_STATIC_TRANSLATED;
	}

	int load_allocation_tag(std::uint64_t generation, std::uint64_t address,
		std::uint8_t *tag)
	{
		if (tag == nullptr)
			return DARWIN_UML_STATIC_HOST_ERROR;
		return access_allocation_tag(generation, address, false, tag);
	}

	int store_allocation_tag(std::uint64_t generation, std::uint64_t address,
		std::uint8_t tag)
	{
		return access_allocation_tag(generation, address, true, &tag);
	}

	[[nodiscard]] std::uint64_t changes(std::uint64_t generation,
					       bool code)
	{
		auto &active = active_thread_generation();
		/* The active generation owns this state for the lifetime of the
		 * thread's activation.  Read its counters in place: copying the
		 * shared_ptr here made every translated block contend on the same
		 * reference count in multi-process workloads.
		 */
		if (active.generation == generation && active.state)
			return (code ? active.state->code_changes :
				active.state->mapping_changes)
				.load(std::memory_order_acquire);
		auto state = state_for(generation);
		return (code ? state->code_changes : state->mapping_changes)
			.load(std::memory_order_acquire);
	}

	[[nodiscard]] darwin_uml_static_sequences sequences(
		std::uint64_t generation)
	{
		auto &active = active_thread_generation();
		if (active.generation == generation && active.state)
		return {
				active.state->mapping_changes.load(
					std::memory_order_acquire),
				active.state->code_changes.load(
					std::memory_order_acquire),
				0,
			};
		auto state = state_for(generation);
		return {
			state->mapping_changes.load(std::memory_order_acquire),
			state->code_changes.load(std::memory_order_acquire),
			0,
		};
	}

	int mapping_changes(std::uint64_t generation, std::uint64_t sequence,
		struct darwin_uml_static_mapping_change *output,
		std::size_t capacity, std::uint64_t *current_sequence)
	{
		if (current_sequence == nullptr || (capacity != 0 && output == nullptr))
			return -1;
		auto state = state_for(generation);
		std::shared_lock lock{state->mappings_mutex};
		const auto current = state->mapping_changes.load(
			std::memory_order_acquire);
		*current_sequence = current;
		if (sequence >= current)
			return 0;
		if (state->mapping_history.empty() ||
		    sequence + 1 < state->mapping_history.front().sequence)
			return -1;
		const auto first_sequence =
			state->mapping_history.front().sequence;
		const auto first_index = sequence < first_sequence ? 0 :
			static_cast<std::size_t>(sequence - first_sequence + 1);
		if (state->mapping_history.size() - first_index > capacity)
			return -1;
		std::size_t count = 0;
		for (auto index = first_index;
		     index < state->mapping_history.size(); ++index) {
			const auto &change = state->mapping_history[index];
			if (change.length == 0)
				return -1;
			output[count++] = {change.address, change.length};
		}
		return static_cast<int>(count);
	}

	void invalidate_code(std::uint64_t address)
	{
		/* Translated blocks are keyed by the address-space code sequence.
		 * Cache maintenance invalidates the current process without making
		 * unrelated processes discard their translated programs.
		 */
		const auto generation = generation_for(
			address, active_thread_generation().generation);
		state_for(generation)->code_changes.fetch_add(
			1, std::memory_order_release);
	}

	[[nodiscard]] std::uint64_t active_generation() const noexcept
	{
		return active_thread_generation().generation;
	}

	void forget_descriptor(int fd)
	{
		std::lock_guard lock{backing_mutex_};
		descriptor_backings_.erase(fd);
	}

	int begin_batch(std::uint64_t generation)
	{
		auto &batch = mapping_batch();
		if (batch.depth != 0) {
			if (batch.generation != generation)
				return -1;
			++batch.depth;
			return 0;
		}
		batch.backings.clear();
		batch.operations.clear();
		batch.generation = generation;
		batch.invalidation_epoch = state_for(batch.generation)->
			invalidation_epoch.load(std::memory_order_acquire);
		batch.depth = 1;
		return 0;
	}

	int end_batch()
	{
		auto &batch = mapping_batch();
		if (batch.depth == 0)
			return -1;
		if (--batch.depth != 0)
			return 0;

		int result = 0;
		auto state = state_for(batch.generation);
		{
			ExecutionQuiesce quiesce{state, batch.generation};
			if (state->invalidation_epoch.load(std::memory_order_acquire) !=
			    batch.invalidation_epoch) {
				result = -EAGAIN;
			} else {
				std::lock_guard lock{state->mappings_mutex};
				for (const auto &operation : batch.operations) {
					if (operation.map)
						map_locked(*state,
							operation.address,
							operation.length,
							operation.mapping);
					else
						unmap_locked(*state,
							operation.address,
							operation.length);
				}
			}
		}
		batch.operations.clear();
		batch.backings.clear();
		batch.generation = 0;
		batch.invalidation_epoch = 0;
		return result;
	}

	int lock_walker(std::uint64_t generation)
	{
		auto &active = active_walker();
		if (active.state)
			return -1;
		auto state = state_for(generation);
		state->walker_mutex.lock();
		active.generation = generation;
		active.state = std::move(state);
		return 0;
	}

	int unlock_walker(std::uint64_t generation)
	{
		auto &active = active_walker();
		if (!active.state || active.generation != generation)
			return -1;
		auto state = std::move(active.state);
		active.generation = 0;
		state->walker_mutex.unlock();
		return 0;
	}

	int execution_begin(std::uint64_t generation, std::uint64_t guest_pc)
	{
		auto &execution = active_thread_execution();
		if (execution.state != nullptr)
			return -1;
		auto &active = active_thread_generation();
		if (active.generation == generation && active.state) {
			execution.state = active.state.get();
		} else {
			execution.owner = state_for(generation);
			execution.state = execution.owner.get();
		}
		if (!execution_reader_begin(
				*execution.state, generation, guest_pc)) {
			execution.state = nullptr;
			execution.owner.reset();
			return -1;
		}
		execution.generation = generation;
		return 0;
	}

	int execution_end(std::uint64_t generation)
	{
		auto &execution = active_thread_execution();
		if (execution.state == nullptr || execution.generation != generation)
			return -1;
		execution_reader_end(*execution.state);
		execution.state = nullptr;
		execution.owner.reset();
		execution.generation = 0;
		return 0;
	}

	ExecutionContext *create_execution_context(std::uint64_t generation)
	{
		auto state = state_for(generation);
		return new (std::nothrow) ExecutionContext{
			std::move(state), generation, 0, false};
	}

	static void destroy_execution_context(ExecutionContext *context)
	{
		if (context == nullptr)
			return;
		if (context->entered)
			execution_reader_end(*context->state);
		delete context;
	}

	static int execution_context_begin(ExecutionContext *context,
					   std::uint64_t guest_pc)
	{
		if (context == nullptr || context->entered)
			return -1;
		/* Capture before entering the gate.  If a writer closes the gate in
		 * between, this reader either waits for it or observes its interrupt;
		 * it can never miss a shootdown that is waiting on this reader.
		 */
		context->execution_interrupts = context->state->execution_interrupts.load(
			std::memory_order_acquire);
		if (!execution_reader_begin(*context->state, context->generation,
					    guest_pc))
			return -1;
		context->entered = true;
		return 0;
	}

	static int execution_context_end(ExecutionContext *context)
	{
		if (context == nullptr || !context->entered)
			return -1;
		execution_reader_end(*context->state);
		context->entered = false;
		return 0;
	}

	static darwin_uml_static_sequences execution_context_sequences(
		const ExecutionContext *context)
	{
		if (context == nullptr)
			return {};
		return {
			context->state->mapping_changes.load(std::memory_order_acquire),
			context->state->code_changes.load(std::memory_order_acquire),
			context->state->execution_interrupts.load(
				std::memory_order_acquire) !=
				context->execution_interrupts,
		};
	}

private:
	int access_allocation_tag(std::uint64_t generation,
		std::uint64_t address, bool write, std::uint8_t *tag)
	{
		/* MTE allocation tags are indexed by the untagged Tag Granule. */
		address &= 0x00ff'ffff'ffff'fff0ULL;
		const auto page = address & ~(page_size - 1);
		const auto resolved_generation = generation_for(page, generation);
		auto reference = reference_for(resolved_generation);
		std::shared_lock lock{reference.state->mappings_mutex};
		const auto iterator = reference.state->mappings.find(page);
		if (iterator == reference.state->mappings.end())
			return DARWIN_UML_STATIC_UNMAPPED;
		auto &mapping = iterator->second;
		const int required = write ? PROT_WRITE : PROT_READ;
		if ((mapping.protection & required) == 0)
			return DARWIN_UML_STATIC_PERMISSION;
		const auto offset = mapping.offset +
			(address & (page_size - 1));
		if (offset >= mapping.backing->size())
			return DARWIN_UML_STATIC_HOST_ERROR;
		if (write) {
			if (!mapping.backing->store_allocation_tag(offset, *tag))
				return DARWIN_UML_STATIC_HOST_ERROR;
		} else {
			*tag = mapping.backing->load_allocation_tag(offset);
		}
		return DARWIN_UML_STATIC_TRANSLATED;
	}

	static std::uint64_t generation_for(std::uint64_t address,
					    std::uint64_t generation) noexcept
	{
		return address < address_limit ? generation : 0;
	}

	GenerationReference reference_for(std::uint64_t generation)
	{
		auto &active = active_thread_generation();
		if (active.generation == generation && active.state)
			return {{}, active.state.get()};
		auto owner = state_for(generation);
		auto *state = owner.get();
		return {std::move(owner), state};
	}

	std::shared_ptr<MappedBacking> backing_for(int fd)
	{
		auto &batch = mapping_batch();
		if (batch.depth != 0) {
			if (auto found = batch.backings.find(fd);
			    found != batch.backings.end())
				return found->second;
		}
		{
			std::lock_guard lock{backing_mutex_};
			if (auto found = descriptor_backings_.find(fd);
			    found != descriptor_backings_.end()) {
				if (auto backing = found->second.lock())
					return backing;
				descriptor_backings_.erase(found);
			}
		}
		struct stat status {};
		if (fstat(fd, &status) != 0)
			return {};
		const BackingKey key{static_cast<std::uint64_t>(status.st_dev),
				     static_cast<std::uint64_t>(status.st_ino)};
		std::lock_guard lock{backing_mutex_};
		if (auto found = backings_.find(key); found != backings_.end()) {
			if (auto backing = found->second.lock()) {
				descriptor_backings_.insert_or_assign(fd, backing);
				if (batch.depth != 0)
					batch.backings.insert_or_assign(fd, backing);
				return backing;
			}
		}
		auto backing = MappedBacking::create(fd, status);
		if (backing) {
			backings_.insert_or_assign(key, backing);
			descriptor_backings_.insert_or_assign(fd, backing);
			if (batch.depth != 0)
				batch.backings.insert_or_assign(fd, backing);
		}
		return backing;
	}

	std::shared_ptr<GenerationState> state_for_locked(
		std::uint64_t generation)
	{
		if (const auto found = states_.find(generation);
		    found != states_.end())
			return found->second;
		auto state = std::make_shared<GenerationState>();
		states_.emplace(generation, state);
		return state;
	}

	std::shared_ptr<GenerationState> state_for(std::uint64_t generation)
	{
		{
			std::shared_lock lock{states_mutex_};
			if (const auto found = states_.find(generation);
			    found != states_.end())
				return found->second;
		}
		std::lock_guard lock{states_mutex_};
		return state_for_locked(generation);
	}

	void map_locked(GenerationState &state, std::uint64_t address,
		std::uint64_t length, const Mapping &mapping)
	{
		bool changed = false;
		bool changes_code = false;
		for (std::uint64_t position = 0; position < length;
		     position += page_size) {
			if (const auto previous = state.mappings.find(address + position);
			    previous != state.mappings.end()) {
				if (previous->second.fd == mapping.fd &&
				    previous->second.offset == mapping.offset + position &&
				    previous->second.protection == mapping.protection &&
				    previous->second.guest_code == mapping.guest_code &&
				    previous->second.guest_writable ==
					mapping.guest_writable)
					continue;
				changes_code = changes_code || previous->second.guest_code;
			}
			changed = true;
			changes_code = changes_code || mapping.guest_code;
			state.mappings.insert_or_assign(
				address + position,
				Mapping{mapping.backing, mapping.offset + position,
					mapping.fd, mapping.protection,
					mapping.guest_code, mapping.guest_writable});
		}
		if (changed)
			changed_locked(state, changes_code, address, length);
	}

	void unmap_locked(GenerationState &state, std::uint64_t address,
		std::uint64_t length)
	{
		bool changed = false;
		bool changes_code = false;
		for (std::uint64_t position = 0; position < length;
		     position += page_size) {
			auto mapping = state.mappings.find(address + position);
			if (mapping == state.mappings.end())
				continue;
			changed = true;
			changes_code = changes_code || mapping->second.guest_code;
			state.mappings.erase(mapping);
		}
		if (changed)
			changed_locked(state, changes_code, address, length);
	}

	void changed_locked(GenerationState &state, bool changes_code,
		std::uint64_t address, std::uint64_t length)
	{
		const auto sequence = state.mapping_changes.fetch_add(
			1, std::memory_order_release) + 1;
		state.mapping_history.push_back({sequence, address, length});
		if (state.mapping_history.size() > maximum_mapping_history)
			state.mapping_history.pop_front();
		if (changes_code)
			state.code_changes.fetch_add(1, std::memory_order_release);
	}

	mutable std::shared_mutex states_mutex_;
	std::mutex backing_mutex_;
	std::unordered_map<std::uint64_t, std::shared_ptr<GenerationState>> states_;
	std::unordered_map<BackingKey, std::weak_ptr<MappedBacking>, BackingKeyHash>
		backings_;
	std::unordered_map<int, std::weak_ptr<MappedBacking>> descriptor_backings_;
};

AddressSpace &address_space()
{
	static AddressSpace instance;
	return instance;
}

} // namespace
} // namespace uml::runtime

extern "C" int darwin_uml_close(int fd)
{
	uml::runtime::address_space().forget_descriptor(fd);
	return ::close(fd);
}

extern "C" int darwin_uml_static_vm_map(void *address, int fd,
					 unsigned long long offset,
					 unsigned long length, int protection,
					 int guest_code, int guest_writable)
{
	return uml::runtime::address_space().map(
		uml::runtime::address_space().active_generation(),
		reinterpret_cast<std::uintptr_t>(address), fd, offset, length,
		protection, guest_code != 0, guest_writable != 0);
}

extern "C" int darwin_uml_static_vm_map_generation(
	unsigned long generation, void *address, int fd,
	unsigned long long offset, unsigned long length, int protection,
	int guest_code, int guest_writable)
{
	return uml::runtime::address_space().map(
		generation, reinterpret_cast<std::uintptr_t>(address), fd, offset,
		length, protection, guest_code != 0, guest_writable != 0);
}

extern "C" int darwin_uml_static_vm_protect(void *address,
					     unsigned long length,
					     int protection)
{
	return uml::runtime::address_space().protect(
		reinterpret_cast<std::uintptr_t>(address), length, protection);
}

extern "C" int darwin_uml_static_vm_unmap(void *address,
					   unsigned long length)
{
	auto &space = uml::runtime::address_space();
	return space.unmap(space.active_generation(),
		reinterpret_cast<std::uintptr_t>(address), length);
}

extern "C" int darwin_uml_static_vm_unmap_generation(
	unsigned long generation, void *address, unsigned long length)
{
	return uml::runtime::address_space().unmap(
		generation, reinterpret_cast<std::uintptr_t>(address), length);
}

extern "C" int darwin_uml_static_vm_activate(unsigned long generation)
{
	return uml::runtime::address_space().activate(generation);
}

extern "C" int darwin_uml_static_vm_destroy(unsigned long generation)
{
	return uml::runtime::address_space().destroy(generation);
}

extern "C" int darwin_uml_static_vm_mapping_matches(
	unsigned long address, int fd, unsigned long long offset)
{
	return uml::runtime::address_space().matches(address, fd, offset);
}

extern "C" int darwin_uml_static_vm_rebind(unsigned long address, int fd,
					      unsigned long long offset)
{
	return uml::runtime::address_space().rebind(address, fd, offset);
}

extern "C" int darwin_uml_static_vm_has_mapping(unsigned long address)
{
	return uml::runtime::address_space().has_mapping(address);
}

extern "C" int darwin_uml_static_vm_copy_from_buffer(
	unsigned long address, const void *buffer, unsigned long length)
{
	return uml::runtime::address_space().copy_from_buffer(
		address, buffer, length);
}

extern "C" int darwin_uml_static_vm_retry_fault(
	unsigned long address, int execute, int write)
{
	auto &space = uml::runtime::address_space();
	void *translated = nullptr;
	const auto access = execute != 0 ? DARWIN_UML_STATIC_EXECUTE :
		write != 0 ? DARWIN_UML_STATIC_WRITE : DARWIN_UML_STATIC_READ;
	const int result = space.translate(
		space.active_generation(), address, access, &translated);
	if (result == DARWIN_UML_STATIC_TRANSLATED && translated != nullptr)
		return 1;
	return result == DARWIN_UML_STATIC_HOST_ERROR ? -1 : 0;
}

extern "C" int darwin_uml_static_vm_batch_begin(unsigned long generation)
{
	return uml::runtime::address_space().begin_batch(generation);
}

extern "C" int darwin_uml_static_vm_batch_end(void)
{
	return uml::runtime::address_space().end_batch();
}

extern "C" int darwin_uml_static_vm_walker_lock(unsigned long generation)
{
	return uml::runtime::address_space().lock_walker(generation);
}

extern "C" int darwin_uml_static_vm_walker_unlock(unsigned long generation)
{
	return uml::runtime::address_space().unlock_walker(generation);
}

extern "C" int darwin_uml_static_vm_translate(
	unsigned long generation, unsigned long address,
	enum darwin_uml_static_access access, void **host_address)
{
	return uml::runtime::address_space().translate(
		generation, address, access, host_address);
}

extern "C" int darwin_uml_static_vm_load_allocation_tag(
	unsigned long generation, unsigned long address, uint8_t *tag)
{
	return uml::runtime::address_space().load_allocation_tag(
		generation, address, tag);
}

extern "C" int darwin_uml_static_vm_store_allocation_tag(
	unsigned long generation, unsigned long address, uint8_t tag)
{
	return uml::runtime::address_space().store_allocation_tag(
		generation, address, tag);
}

extern "C" uint64_t darwin_uml_static_vm_changes(unsigned long generation)
{
	return uml::runtime::address_space().changes(generation, false);
}

extern "C" struct darwin_uml_static_sequences darwin_uml_static_vm_sequences(
	unsigned long generation)
{
	return uml::runtime::address_space().sequences(generation);
}

extern "C" int darwin_uml_static_vm_mapping_changes(
	unsigned long generation, uint64_t sequence,
	struct darwin_uml_static_mapping_change *changes, size_t capacity,
	uint64_t *current_sequence)
{
	return uml::runtime::address_space().mapping_changes(
		generation, sequence, changes, capacity, current_sequence);
}

extern "C" int darwin_uml_static_vm_execution_begin(
	unsigned long generation, unsigned long guest_pc)
{
	return uml::runtime::address_space().execution_begin(
		generation, guest_pc);
}

extern "C" int darwin_uml_static_vm_execution_end(
	unsigned long generation)
{
	return uml::runtime::address_space().execution_end(generation);
}

extern "C" void *darwin_uml_static_vm_execution_context_create(
	unsigned long generation)
{
	return uml::runtime::address_space().create_execution_context(generation);
}

extern "C" void darwin_uml_static_vm_execution_context_destroy(void *context)
{
	uml::runtime::address_space().destroy_execution_context(
		static_cast<uml::runtime::ExecutionContext *>(context));
}

extern "C" int darwin_uml_static_vm_execution_context_begin(
	void *context, unsigned long guest_pc)
{
	return uml::runtime::address_space().execution_context_begin(
		static_cast<uml::runtime::ExecutionContext *>(context), guest_pc);
}

extern "C" int darwin_uml_static_vm_execution_context_end(void *context)
{
	return uml::runtime::address_space().execution_context_end(
		static_cast<uml::runtime::ExecutionContext *>(context));
}

extern "C" struct darwin_uml_static_sequences
darwin_uml_static_vm_execution_context_sequences(void *context)
{
	return uml::runtime::address_space().execution_context_sequences(
		static_cast<uml::runtime::ExecutionContext *>(context));
}

extern "C" uint64_t darwin_uml_static_vm_code_changes(
	unsigned long generation)
{
	return uml::runtime::address_space().changes(generation, true);
}

extern "C" void darwin_uml_static_vm_invalidate_code(unsigned long address)
{
	uml::runtime::address_space().invalidate_code(address);
}
