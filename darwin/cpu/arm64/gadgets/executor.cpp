// SPDX-License-Identifier: GPL-2.0-or-later
#include "executor.hpp"

#include "cpu/arm64/detail/floating_exception.hpp"
#include "shared_programs.hpp"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uml::cpu::arm64::gadgets {
using ::uml::memory::StaticGuestMemory;
namespace {


#ifndef UML_GADGET_BLOCK_INSTRUCTIONS
#define UML_GADGET_BLOCK_INSTRUCTIONS 64
#endif
constexpr std::size_t block_instruction_count = UML_GADGET_BLOCK_INSTRUCTIONS;
constexpr std::size_t guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE;

[[nodiscard]] std::size_t fetch_instruction_window(
	StaticGuestMemory &memory, std::uint64_t pc,
	std::span<std::uint32_t> instructions)
{
	std::size_t count = 0;
	auto bytes = std::as_writable_bytes(instructions);
	while (count < instructions.size()) {
		const auto address = pc + count * sizeof(std::uint32_t);
		const auto page_remaining =
			guest_page_size - (address & (guest_page_size - 1));
		auto page_instructions = std::min<std::size_t>(
			instructions.size() - count,
			static_cast<std::size_t>(
				page_remaining / sizeof(std::uint32_t)));
		/* A valid A64 PC is word aligned. Preserve deterministic fault
		 * handling for a malformed PC instead of constructing an empty span.
		 */
		if (page_instructions == 0)
			page_instructions = 1;
		const auto offset = count * sizeof(std::uint32_t);
		const auto length = page_instructions * sizeof(std::uint32_t);
		if (!memory.fetch(address, bytes.subspan(offset, length)))
			break;
		count += page_instructions;
	}
	return count;
}

[[nodiscard]] constexpr bool terminal_does_not_retire(
	::uml::cpu::EngineEvent event) noexcept
{
	switch (event) {
	case ::uml::cpu::EngineEvent::fatal:
	case ::uml::cpu::EngineEvent::memory_fault:
	case ::uml::cpu::EngineEvent::illegal_instruction:
	case ::uml::cpu::EngineEvent::gcs_fault:
	case ::uml::cpu::EngineEvent::floating_point_trap:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] bool program_contains_stream(
	const arm64::gadgets::Program &program, const void *stream) noexcept
{
	if (stream == nullptr || program.words().empty())
		return false;
	const auto marker = reinterpret_cast<std::uintptr_t>(stream);
	const auto begin = reinterpret_cast<std::uintptr_t>(
		program.words().data());
	const auto end =
		begin + program.words().size() * sizeof(std::uintptr_t);
	return marker >= begin && marker < end;
}

[[nodiscard]] std::uint32_t effective_fpcr(
	const arm64::CpuState &cpu) noexcept
{
	return cpu.fp_loaded ? cpu.fpcr : cpu.deferred_fpcr;
}

void correct_terminal_instruction_count(
	const arm64::gadgets::Program &program, ::uml::cpu::EngineEvent event,
	std::uint64_t fault_pc, std::size_t &executed) noexcept
{
	if (!terminal_does_not_retire(event))
		return;

	const auto base = program.guest_address();
	if (fault_pc < base)
		return;
	const auto offset = fault_pc - base;
	if ((offset & (sizeof(std::uint32_t) - 1U)) != 0U)
		return;
	const auto fault_index = offset / sizeof(std::uint32_t);
	if (fault_index >= program.instruction_count())
		return;
	const auto unretired =
		program.instruction_count() - static_cast<std::size_t>(fault_index);
	if (executed < unretired)
		return;

	/* The assembly engine precharges a block's full translated length.  A
	 * non-retiring terminal stores its own PC, allowing the cold C++ boundary
	 * to subtract the current block's unexecuted suffix. Counts accumulated
	 * before a linked successor are retained.
	 */
	executed -= unretired;
}

void correct_fpcr_enable_instruction_count(
	const arm64::gadgets::Program &program, ::uml::cpu::EngineEvent event,
	const arm64::CpuState &cpu, std::size_t &executed) noexcept
{
	if (event != ::uml::cpu::EngineEvent::continued ||
	    (effective_fpcr(cpu) &
	     arm64::detail::fpcr_exception_enable_mask) == 0U)
		return;

	const auto base = program.guest_address();
	if (cpu.pc <= base)
		return;
	const auto offset = cpu.pc - base;
	if ((offset & (sizeof(std::uint32_t) - 1U)) != 0U)
		return;
	const auto retired = offset / sizeof(std::uint32_t);
	if (retired > program.instruction_count())
		return;
	const auto unretired =
		program.instruction_count() - static_cast<std::size_t>(retired);
	if (executed < unretired)
		return;

	/* An FPCR write that newly enables traps exits at its translated
	 * fallthrough PC. It retires, but later words in the precharged block do
	 * not. Unresolved control-flow exits cannot satisfy the enabled-state
	 * condition because enabled entries use the one-instruction cold path.
	 */
	executed -= unretired;
}

/* Which translations this executor's builds are interchangeable with.
 *
 * Gadget selection reads every one of these, so two executors configured
 * differently must not adopt each other's streams even for identical guest
 * words. Production binds one profile to the whole pool, and only tests vary
 * them, but a silent collision here would be a wrong translation rather than a
 * lost opportunity.
 */
[[nodiscard]] std::uint64_t profile_digest(
	const arm64::gadgets::BuildOptions &options) noexcept
{
	static_assert(sizeof(arm64::gadgets::BuildOptions) ==
		sizeof(::uml::cpu::HostCapabilities) + 16,
		"a new build option must be added to the profile digest below, "
		"or executors with different profiles will share translations");
	const auto &host = options.host_capabilities;
	const bool selectors[] = {
		options.enable_pointer_authentication,
		options.enable_guarded_control_flow,
		options.force_software_checksum,
		options.force_software_flags,
		options.force_software_sha3,
		options.force_software_fixed_transforms,
		options.force_software_aes,
		options.force_software_pmull,
		options.force_software_dotprod,
		options.force_software_i8mm,
		host.floating_point,
		host.advanced_simd,
		host.fp16,
		host.cssc,
		host.aes,
		host.pmull,
		host.sha1,
		host.sha2,
		host.crc32,
		host.sha3,
		host.sha512,
		host.afp,
		host.jscvt_subnormal_flags_correct,
	};
	auto digest = static_cast<std::uint64_t>(
		std::hash<::uml::platform::ArmFeatureSet>{}(host.supported));
	for (const bool selector : selectors)
		digest = (digest ^ static_cast<std::uint64_t>(selector)) *
			0x100000001b3ULL;
	return digest;
}

} // namespace

#ifndef UML_GADGET_CACHE_SLOTS
#define UML_GADGET_CACHE_SLOTS 16384
#endif
#ifndef UML_GADGET_RESIDENT_GENERATIONS
#define UML_GADGET_RESIDENT_GENERATIONS 8
#endif
constexpr std::size_t uml_gadget_cache_slots = UML_GADGET_CACHE_SLOTS;
/* How many address spaces may hold translations at once.
 *
 * Sharing one pool without a bound is not a cache: a guest that starts and
 * exits processes accumulates every address space it ever ran, and the handful
 * that are actually live end up with a few dozen slots each. Reuse measured on
 * a browser guest returns to one of the last eight distinct address spaces
 * 94.5% of the time, so bounding residency there keeps capacity for the ones
 * still running while costing almost no returns.
 */
constexpr std::size_t uml_gadget_resident_generations =
	UML_GADGET_RESIDENT_GENERATIONS;

extern "C" void uml_arm64_gadget_link_exit();
extern "C" void uml_arm64_gadget_fp_link_entry();

class CountingResource final : public std::pmr::memory_resource {
public:
	[[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

private:
	void *do_allocate(std::size_t bytes, std::size_t alignment) override
	{
		bytes_ += bytes;
		return std::pmr::new_delete_resource()->allocate(bytes, alignment);
	}

	void do_deallocate(void *pointer, std::size_t bytes,
			   std::size_t alignment) override
	{
		bytes_ -= bytes;
		std::pmr::new_delete_resource()->deallocate(
			pointer, bytes, alignment);
	}

	[[nodiscard]] bool do_is_equal(
		const std::pmr::memory_resource &other) const noexcept override
	{
		return this == &other;
	}

	std::size_t bytes_{};
};

class GadgetExecutor::ProgramCache final {
public:
	ProgramCache()
		: program_storage_{&counted_},
		  stream_index_{std::make_unique<StreamIndexSet[]>(
			  stream_index_set_count)}
	{}

	struct Footprint final {
		std::size_t stream_index, slabs, streams, links, indirect;
		[[nodiscard]] std::size_t total() const noexcept
		{
			return stream_index + slabs + streams + links + indirect;
		}
	};

	[[nodiscard]] Footprint footprint() const noexcept
	{
		std::size_t slabs = 0;
		for (const auto &slab : slabs_)
			if (slab)
				slabs += sizeof(Slab);
		/* libc++ keeps one heap node per element plus a bucket pointer;
		 * close enough to attribute the map, exact for the arrays. */
		constexpr std::size_t node = 2 * sizeof(void *);
		return {
			.stream_index = stream_index_set_count *
				sizeof(StreamIndexSet),
			.slabs = slabs,
			.streams = counted_.bytes(),
			.links = links_.size() *
				(sizeof(LinkRecord) + node) +
				links_.bucket_count() * sizeof(void *),
			.indirect = indirect_links_.size() *
				(sizeof(arm64::gadgets::IndirectLink) + node) +
				indirect_links_.bucket_count() * sizeof(void *),
		};
	}

	/* How many address spaces currently hold translations here. With one
	 * shared pool this is an outcome of the workload, not a build option. */
	[[nodiscard]] std::size_t resident_generations() const noexcept
	{
		return generation_changes_.size();
	}

	[[nodiscard]] std::size_t occupancy() const noexcept
	{
		std::size_t occupied = 0;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots)
				if (slot.epoch == epoch_ && slot.result)
					++occupied;
		}
		return occupied;
	}

	void invalidate() noexcept
	{
		live_links_.clear();
		live_indirect_.clear();
		resident_.clear();
		generation_changes_.clear();
		indirect_links_.clear();
		links_.clear();
		++epoch_;
		if (epoch_ != 0)
			return;
		for (auto &slab : slabs_)
			if (slab)
				for (auto &slot : slab->slots)
					slot.epoch = 0;
		for (std::size_t set = 0; set < stream_index_set_count; ++set)
			for (auto &entry : stream_index_[set].entries)
				entry = {};
		epoch_ = 1;
	}

	/* Reset the activations of one address space, as its code change
	 * requires.
	 *
	 * The code-change sequence is per address space, so a change in one says
	 * nothing about the others and their links stay live. Within the changed
	 * one, only a record that was activated can differ from the reset state,
	 * so the walk is over those rather than over every record the translator
	 * ever named -- with several address spaces resident, that difference is
	 * the whole cost of the reset.
	 */
	void invalidate_links(unsigned long generation) noexcept
	{
		std::size_t kept = 0;
		for (auto *record : live_links_) {
			if (record->generation != generation) {
				live_links_[kept++] = record;
				continue;
			}
			record->reset();
			record->listed = false;
		}
		live_links_.resize(kept);
		if (live_indirect_.empty())
			return;
		const auto *empty_link = &link_record(empty_pc).link;
		kept = 0;
		for (const auto &enrolled : live_indirect_) {
			if (enrolled.first != generation) {
				live_indirect_[kept++] = enrolled;
				continue;
			}
			for (auto &entry : enrolled.second->entries) {
				entry.guest_pc = empty_pc;
				entry.link = empty_link;
			}
			enrolled.second->next_replacement = 0;
		}
		live_indirect_.resize(kept);
	}

	/* Bind the cache to an address space and answer for whatever changed in
	 * it while it was away. Each address space carries its own mark, so a
	 * switch back to one whose code never changed costs nothing. */
	void prepare(unsigned long generation, std::uint64_t changes)
	{
		current_generation_ = generation;
		auto [entry, inserted] =
			generation_changes_.try_emplace(generation, changes);
		touch(generation);
		if (inserted) {
			while (resident_.size() > uml_gadget_resident_generations)
				retire(resident_.front());
			return;
		}
		if (entry->second == changes)
			return;
		invalidate_links(generation);
		entry->second = changes;
	}

	[[nodiscard]] const BuildResult *find(
		std::uint64_t pc, std::uint64_t changes,
		std::uint64_t mapping_changes, StaticGuestMemory &memory,
		void *execution_context, GadgetCacheDiagnostics &diagnostics)
	{
		const auto location = locate(current_generation_, pc);
		auto &slab = slabs_[location.slab];
		if (!slab) {
			return nullptr;
		}
		for (std::size_t way = 0; way < ways; ++way) {
			auto &slot = slab->slots[location.offset + way];
			if (slot.epoch != epoch_ || slot.pc != pc ||
			    slot.generation != current_generation_ || !slot.result)
				continue;
#if UML_GADGET_EXACT_CACHE_VALIDATION
			if (slot.validated_changes == changes) {
				return &*slot.result;
			}
			++diagnostics.validation_attempts;
			if (!validate(slot, changes, mapping_changes, memory,
				    execution_context, diagnostics)) {
				return nullptr;
			}
			activate(pc, slot);
			return &*slot.result;
#else
			(void)changes;
			(void)mapping_changes;
			(void)memory;
			(void)execution_context;
			(void)diagnostics;
			return &*slot.result;
#endif
		}
		return nullptr;
	}

	[[nodiscard]] const BuildResult *find_stream(
		const void *stream) const noexcept
	{
		if (stream == nullptr)
			return nullptr;
		const auto marker = reinterpret_cast<std::uintptr_t>(stream);
		const auto &set = stream_index_[stream_index_location(marker)];
		for (const auto &entry : set.entries) {
			const auto *slot = entry.slot;
			if (slot != nullptr && slot->epoch == epoch_ &&
			    slot->activation == entry.activation &&
			    slot->result &&
			    program_contains_stream(slot->result->program, stream))
				return &*slot->result;
		}

		/*
		 * The fixed index deliberately permits replacement under unusual
		 * host-address collisions. Preserve exact terminal accounting with
		 * a cold correctness fallback; ordinary linked exits find one of
		 * four allocation-free entries above.
		 */
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots)
				if (slot.epoch == epoch_ && slot.result &&
				    program_contains_stream(
					    slot.result->program, stream))
					return &*slot.result;
		}
		return nullptr;
	}

	[[nodiscard]] const arm64::gadgets::BlockLink *link(
		std::uint64_t pc)
	{
		return &link_record(pc).link;
	}

	[[nodiscard]] arm64::gadgets::IndirectLink *indirect_link(
		std::uint64_t source_pc)
	{
		const auto [created, inserted] =
			indirect_links_.try_emplace(
				LinkKey{current_generation_, source_pc});
		if (inserted) {
			const auto *empty_link = link(empty_pc);
			for (auto &entry : created->second.entries) {
				entry.guest_pc = empty_pc;
				entry.link = empty_link;
			}
		}
		return &created->second;
	}

	void learn_indirect(arm64::gadgets::IndirectLink &site,
			    std::uint64_t target_pc)
	{
		const auto *target = link(target_pc);
		/* Training is the only thing that moves a site off its empty
		 * cells, and empty_pc is unreachable as a guest target, so a
		 * site still holding two empty cells is untrained since its last
		 * reset -- which makes this the transition that enrols it. */
		if (site.entries[0].guest_pc == empty_pc &&
		    site.entries[1].guest_pc == empty_pc)
			live_indirect_.emplace_back(current_generation_, &site);
		for (auto &entry : site.entries) {
			if (entry.guest_pc != target_pc)
				continue;
			entry.link = target;
			return;
		}
		auto &entry = site.entries[site.next_replacement & 1U];
		entry.guest_pc = target_pc;
		entry.link = target;
		site.next_replacement ^= 1U;
	}

	/* Take a translation another executor built, if one is on offer.
	 *
	 * The canonical stream holds zero wherever the producer held a pointer
	 * to its own link state. Resolving those cells against this cache, while
	 * copying the stream into this cache's storage, is the whole of what
	 * makes the translation ours: after this the block is indistinguishable
	 * from one built here, and the producer may be destroyed without
	 * touching it.
	 */
	[[nodiscard]] const BuildResult *adopt(
		const arm64::gadgets::SharedKey &key, std::uint64_t changes)
	{
		std::pmr::vector<std::uintptr_t> words{&program_storage_};
		arm64::gadgets::SharedTranslation translation;
		if (!arm64::gadgets::SharedProgramStore::instance().adopt(
			    key, words, translation))
			return nullptr;
		const auto &relocations = translation.relocations;
		for (std::size_t index = 0; index < relocations.count; ++index) {
			const auto &relocation = relocations.entries[index];
			if (relocation.word >= words.size())
				return nullptr;
			words[relocation.word] = relocation.indirect ?
				reinterpret_cast<std::uintptr_t>(
					indirect_link(relocation.guest_pc)) :
				reinterpret_cast<std::uintptr_t>(
					link(relocation.guest_pc));
		}
		return &store(key.guest_pc,
			BuildResult{
				arm64::gadgets::Program{
					key.guest_pc,
					translation.instruction_count,
					std::move(words),
					translation.accesses_fp_state},
				translation.complete,
				translation.stop_address,
				translation.stop_instruction},
			key.window, changes);
	}

	[[nodiscard]] const BuildResult &store(
		std::uint64_t pc, BuildResult result,
		std::span<const std::uint32_t> guest_words,
		std::uint64_t changes)
	{
		const auto location = locate(current_generation_, pc);
		auto &slab = slabs_[location.slab];
		if (!slab)
			slab = std::make_unique<Slab>();

		auto &replacement = next_replacement_[location.set];
		std::size_t selected = replacement;
		for (std::size_t way = 0; way < ways; ++way) {
			const auto &slot = slab->slots[location.offset + way];
			if (slot.epoch != epoch_) {
				selected = way;
				break;
			}
		}
		replacement = static_cast<std::uint8_t>((selected + 1) & (ways - 1));

		auto &slot = slab->slots[location.offset + selected];
		if (slot.epoch == epoch_ && slot.result) {
			deactivate(slot.generation, slot.pc,
				slot.result->program);
		}
		slot.result.emplace(std::move(result));
		slot.pc = pc;
		slot.generation = current_generation_;
		slot.epoch = epoch_;
#if UML_GADGET_EXACT_CACHE_VALIDATION
		slot.validated_changes = changes;
		const auto validation_words =
			slot.result->program.instruction_count() +
			static_cast<std::size_t>(!slot.result->complete);
		if (validation_words != 0 &&
		    validation_words <= block_instruction_count &&
		    validation_words <= guest_words.size()) {
			if (slot.source_word_capacity < validation_words) {
				slot.source_words =
					std::make_unique_for_overwrite<std::uint32_t[]>(
						validation_words);
				slot.source_word_capacity = validation_words;
			}
			std::copy_n(guest_words.begin(), validation_words,
				slot.source_words.get());
			slot.source_word_count = validation_words;
		} else {
			slot.source_word_count = 0;
		}
#else
		(void)guest_words;
		(void)changes;
#endif
		++slot.activation;
		if (slot.activation == 0)
			slot.activation = 1;
		activate(pc, slot);
		return *slot.result;
	}

	[[nodiscard]] std::pmr::memory_resource *program_storage() noexcept
	{
		return &program_storage_;
	}

private:
	static constexpr std::size_t ways = 4;
	static constexpr std::size_t slot_count = uml_gadget_cache_slots;
	static constexpr std::size_t set_count = slot_count / ways;
	static constexpr std::size_t slab_slots = 256;
	static constexpr std::size_t slab_count = slot_count / slab_slots;
	static constexpr unsigned stream_index_granule_shift = 6;
	static constexpr std::size_t stream_index_ways = 4;
	/* The unreachable guest PC an untrained indirect cell names; guest
	 * targets are admitted only word-aligned, so none can collide. */
	static constexpr std::uint64_t empty_pc = ~std::uint64_t{};
	/* One index set per slot, as when both were fixed at 16384. */
	static constexpr std::size_t stream_index_set_count = slot_count;
	static_assert((slot_count & (slot_count - 1)) == 0,
		"cache slot count must be a power of two");
	static_assert(slot_count >= slab_slots * 4,
		"cache must hold at least four slabs");

	struct LinkRecord;

	struct Slot final {
		std::optional<BuildResult> result;
		/* The address space this translation belongs to. A guest virtual
		 * address only means something inside one, so a slot is a match
		 * only when the generation matches too; that is what lets
		 * translations for several address spaces share one cache
		 * instead of being wiped at every switch.
		 */
		unsigned long generation{};
#if UML_GADGET_EXACT_CACHE_VALIDATION
		std::unique_ptr<std::uint32_t[]> source_words;
#endif
		std::uint64_t pc{};
		std::uint64_t epoch{};
		std::uint64_t activation{};
		/* Reactivation resolves the same link record every time, so the
		 * node address is memoised here rather than hashed again on each
		 * pass. links_ is node-based, so the address is stable until
		 * invalidate() clears the map, and that also advances epoch_,
		 * which is what link_epoch is compared against.
		 */
		LinkRecord *link{};
		std::uint64_t link_epoch{};
		/* The activation whose stream index entries are already installed.
		 * store() gives every activation a nonzero value, so the zero this
		 * starts at, and the zero an eviction writes back, both read as
		 * "not indexed".
		 */
		std::uint64_t indexed_activation{};
#if UML_GADGET_EXACT_CACHE_VALIDATION
		std::uint64_t validated_changes{};
		std::size_t source_word_count{};
		std::size_t source_word_capacity{};
#endif
	};

	struct Slab final {
		std::array<Slot, slab_slots> slots;
	};

	struct Location final {
		std::size_t set;
		std::size_t slab;
		std::size_t offset;
	};

	struct StreamIndexEntry final {
		/* A reused cache slot changes activation, invalidating every old
		 * index entry without walking the former program's stream.
		 */
		Slot *slot{};
		std::uint64_t activation{};
	};

	struct StreamIndexSet final {
		std::array<StreamIndexEntry, stream_index_ways> entries;
		std::uint8_t next_replacement{};
	};

	/* A link names a guest PC, which is only meaningful inside one address
	 * space; keying by PC alone would let a block in one branch straight
	 * into another's translation. */
	struct LinkKey final {
		unsigned long generation;
		std::uint64_t pc;
		[[nodiscard]] bool operator==(const LinkKey &) const noexcept =
			default;
	};

	struct LinkKeyHash final {
		[[nodiscard]] std::size_t operator()(
			const LinkKey &key) const noexcept
		{
			const auto mixed = key.pc ^
				(static_cast<std::uint64_t>(key.generation) *
					0x9e3779b97f4a7c15ULL);
			return static_cast<std::size_t>(
				mixed ^ (mixed >> 29));
		}
	};

	struct LinkRecord final {
		explicit LinkRecord(std::uint64_t pc) noexcept
			: fallback_stream{
				reinterpret_cast<std::uintptr_t>(
					uml_arm64_gadget_link_exit),
				static_cast<std::uintptr_t>(pc)},
			  fp_stream{
				reinterpret_cast<std::uintptr_t>(
					uml_arm64_gadget_fp_link_entry),
				0},
			  link{
				.stream = fallback_stream.data(),
				.instruction_count = 0,
				.guest_pc = pc,
			  }
		{
		}

		void reset() noexcept
		{
			link.stream = fallback_stream.data();
			link.instruction_count = 0;
			fp_stream[1] = 0;
		}

		std::array<std::uintptr_t, 2> fallback_stream;
		std::array<std::uintptr_t, 2> fp_stream;
		arm64::gadgets::BlockLink link;
		/* Set while this record sits in live_links_, so a code change
		 * resets the records that hold an activation instead of every
		 * record the translator ever named. */
		unsigned long generation{};
		bool listed{};
	};

	[[nodiscard]] static Location locate(
		unsigned long generation, std::uint64_t pc) noexcept
	{
		/* Fold the address space into the index so the same guest PC in
		 * two address spaces lands in different sets rather than
		 * competing for the four ways of one.
		 */
		const auto block = (pc >> 2) ^
			(static_cast<std::uint64_t>(generation) *
				0x9e3779b97f4a7c15ULL);
		const auto mixed = block ^ (block >> 12) ^ (block >> 24);
		const auto set = static_cast<std::size_t>(mixed) & (set_count - 1);
		const auto first_slot = set * ways;
		return {
			.set = set,
			.slab = first_slot / slab_slots,
			.offset = first_slot % slab_slots,
		};
	}

	[[nodiscard]] static std::size_t stream_index_location(
		std::uintptr_t stream) noexcept
	{
		const auto chunk = stream >> stream_index_granule_shift;
		const auto mixed = chunk ^ (chunk >> 14) ^ (chunk >> 28);
		return static_cast<std::size_t>(mixed) &
			(stream_index_set_count - 1);
	}

	template <typename Operation>
	static void for_each_stream_index_set(
		const arm64::gadgets::Program &program, Operation operation)
	{
		const auto words = program.words();
		if (words.empty())
			return;
		const auto begin = reinterpret_cast<std::uintptr_t>(words.data());
		const auto last = begin +
			words.size() * sizeof(std::uintptr_t) - 1;
		auto chunk = begin >> stream_index_granule_shift;
		const auto last_chunk = last >> stream_index_granule_shift;
		for (;;) {
			operation(stream_index_location(
				chunk << stream_index_granule_shift));
			if (chunk == last_chunk)
				break;
			++chunk;
		}
	}

	/* Install this slot's stream index entries.
	 *
	 * Only store() changes a slot's activation, so a reactivation would
	 * write back exactly the entries the previous pass wrote: same slot
	 * address, same activation, same program. Repeating that walk matters
	 * only where another program has since taken one of the ways for
	 * itself, and that is observable at the moment it happens, so the
	 * evicting pass clears the victim's marker and the victim rebuilds at
	 * its own next activation -- which is when an unconditional walk would
	 * have rebuilt it too. Index coverage is therefore identical either
	 * way, and only the passes that would have changed nothing disappear.
	 */
	void index_stream(Slot &slot)
	{
		if (slot.indexed_activation == slot.activation)
			return;
		const auto &result = *slot.result;
		for_each_stream_index_set(result.program, [&](std::size_t location) {
			auto &set = stream_index_[location];
			std::size_t selected =
				set.next_replacement & (stream_index_ways - 1);
			for (std::size_t way = 0; way < stream_index_ways; ++way) {
				const auto &entry = set.entries[way];
				const bool current =
					entry.slot != nullptr &&
					entry.slot->epoch == epoch_ &&
					entry.slot->activation ==
						entry.activation &&
					entry.slot->result;
				if (!current || entry.slot == &slot) {
					selected = way;
					break;
				}
			}
			auto &chosen = set.entries[selected];
			/* Taking a way from an entry that was still current is
			 * what leaves its owner partially indexed. A way that had
			 * already lapsed costs its owner nothing, and its owner
			 * has moved on to an activation this cannot describe.
			 */
			if (chosen.slot != nullptr && chosen.slot != &slot &&
			    chosen.slot->epoch == epoch_ &&
			    chosen.slot->activation == chosen.activation)
				chosen.slot->indexed_activation = 0;
			chosen = {
				.slot = &slot,
				.activation = slot.activation,
			};
			set.next_replacement = static_cast<std::uint8_t>(
				(selected + 1) & (stream_index_ways - 1));
		});
		slot.indexed_activation = slot.activation;
	}

	/* Least-recently-current first. */
	void touch(unsigned long generation)
	{
		const auto found = std::find(
			resident_.begin(), resident_.end(), generation);
		if (found != resident_.end())
			resident_.erase(found);
		resident_.push_back(generation);
	}

	/* Drop everything describing one address space: its resident
	 * translations, the links the generated code reaches them through, and
	 * its indirect sites. Retirement is rare -- only a first sight of an
	 * address space can force one -- so walking the slabs here costs far
	 * less than leaving dead address spaces holding slots.
	 */
	void retire(unsigned long generation)
	{
		for (auto &slab : slabs_) {
			if (!slab)
				continue;
			for (auto &slot : slab->slots) {
				if (slot.epoch != epoch_ ||
				    slot.generation != generation || !slot.result)
					continue;
				deactivate(slot.generation, slot.pc,
					slot.result->program);
				slot.result.reset();
				slot.epoch = 0;
				slot.link = nullptr;
			}
		}
		std::erase_if(live_links_, [&](const LinkRecord *record) {
			return record->generation == generation;
		});
		std::erase_if(live_indirect_,
			[&](const auto &enrolled) {
				return enrolled.first == generation;
			});
		std::erase_if(links_, [&](const auto &entry) {
			return entry.first.generation == generation;
		});
		std::erase_if(indirect_links_, [&](const auto &entry) {
			return entry.first.generation == generation;
		});
		generation_changes_.erase(generation);
		const auto found = std::find(
			resident_.begin(), resident_.end(), generation);
		if (found != resident_.end())
			resident_.erase(found);
	}

	[[nodiscard]] LinkRecord &link_record(std::uint64_t pc)
	{
		auto [entry, inserted] = links_.try_emplace(
			LinkKey{current_generation_, pc}, pc);
		if (inserted)
			entry->second.generation = current_generation_;
		return entry->second;
	}

#if UML_GADGET_EXACT_CACHE_VALIDATION
	void discard(Slot &slot) noexcept
	{
		if (slot.result)
			deactivate(slot.generation, slot.pc,
				slot.result->program);
		slot.result.reset();
		slot.epoch = 0;
		slot.source_word_count = 0;
		slot.validated_changes = 0;
	}

	[[nodiscard]] bool validate(
		Slot &slot, std::uint64_t changes, std::uint64_t mapping_changes,
		StaticGuestMemory &memory, void *execution_context,
		GadgetCacheDiagnostics &diagnostics)
	{
		if (slot.source_words == nullptr || slot.source_word_count == 0 ||
		    slot.source_word_count > slot.source_word_capacity ||
		    slot.source_word_count > block_instruction_count) {
			++diagnostics.validation_mismatches;
			discard(slot);
			return false;
		}

		std::array<std::uint32_t, block_instruction_count> fetched;
		std::size_t index = 0;
		while (index < slot.source_word_count) {
			const auto address = slot.pc + index * sizeof(std::uint32_t);
			const auto page_remaining =
				guest_page_size - (address & (guest_page_size - 1));
			auto run = std::min<std::size_t>(
				slot.source_word_count - index,
				page_remaining / sizeof(std::uint32_t));
			if (run == 0)
				run = 1;
			auto words = std::span{fetched}.subspan(index, run);
			if (!memory.fetch(address, std::as_writable_bytes(words))) {
				++diagnostics.validation_faults;
				discard(slot);
				return false;
			}
			if (std::memcmp(words.data(), slot.source_words.get() + index,
					run * sizeof(std::uint32_t)) != 0) {
				++diagnostics.validation_mismatches;
				discard(slot);
				return false;
			}
			index += run;
		}

		const auto current =
			darwin_uml_static_vm_execution_context_sequences(
				execution_context);
		if (current.execution_interrupt != 0 ||
		    current.mapping_changes != mapping_changes ||
		    current.code_changes != changes) {
			++diagnostics.validation_sequence_races;
			return false;
		}
		slot.validated_changes = changes;
		++diagnostics.validation_hits;
		return true;
	}
#endif

	void activate(std::uint64_t pc, Slot &slot)
	{
		const auto &result = *slot.result;
		if (!result.complete || result.program.instruction_count() == 0)
			return;
		index_stream(slot);
		if (slot.link == nullptr || slot.link_epoch != epoch_ ||
		    slot.link->link.guest_pc != pc) {
			slot.link = &link_record(pc);
			slot.link_epoch = epoch_;
		}
		auto &record = *slot.link;
		if (!record.listed) {
			record.listed = true;
			live_links_.push_back(&record);
		}
		auto *target = &record.link;
		if (result.program.accesses_fp_state()) {
			record.fp_stream[1] = reinterpret_cast<std::uintptr_t>(
				result.program.words().data());
			target->stream = record.fp_stream.data();
		} else {
			target->stream = result.program.words().data();
		}
		target->instruction_count = result.program.instruction_count();
	}

	void deactivate(unsigned long generation, std::uint64_t pc,
			const arm64::gadgets::Program &program) noexcept
	{
		/* Slots of several address spaces share this cache, so an
		 * eviction is frequently not from the current one; the record to
		 * retire belongs to the slot's address space, not to whichever
		 * happens to be running. */
		const auto found = links_.find(LinkKey{generation, pc});
		if (found == links_.end())
			return;
		auto &record = found->second;
		const auto *words = program.words().data();
		const bool direct = record.link.stream == words;
		const bool fp = record.link.stream == record.fp_stream.data() &&
			record.fp_stream[1] ==
				reinterpret_cast<std::uintptr_t>(words);
		if (direct || fp)
			record.reset();
	}

	CountingResource counted_;
	std::pmr::unsynchronized_pool_resource program_storage_;
	std::unordered_map<LinkKey, LinkRecord, LinkKeyHash> links_;
	std::unordered_map<LinkKey, arm64::gadgets::IndirectLink, LinkKeyHash>
		indirect_links_;
	/* The records holding an activation, and the code-change mark each
	 * address space was last reset for. */
	std::vector<LinkRecord *> live_links_;
	std::vector<std::pair<unsigned long, arm64::gadgets::IndirectLink *>>
		live_indirect_;
	std::unordered_map<unsigned long, std::uint64_t> generation_changes_;
	std::vector<unsigned long> resident_;
	unsigned long current_generation_{};
	std::unique_ptr<StreamIndexSet[]> stream_index_;
	std::array<std::unique_ptr<Slab>, slab_count> slabs_;
	std::array<std::uint8_t, set_count> next_replacement_{};
	std::uint64_t epoch_{1};
};

GadgetExecutor::GadgetExecutor(arm64::gadgets::BuildOptions build_options)
	: programs_{std::make_unique<ProgramCache>()},
	  build_options_{build_options},
	  profile_{profile_digest(build_options)}
{
	arm64::gadgets::SharedProgramStore::executor_created();
}

GadgetExecutor::~GadgetExecutor()
{
	arm64::gadgets::SharedProgramStore::executor_destroyed();
	if (current_cpu_ != nullptr) {
		current_cpu_->gadget_indirect_miss = nullptr;
		arm64::restore_native_fp_environment(*current_cpu_);
	}
	darwin_uml_static_vm_execution_context_destroy(execution_context_);
}

bool GadgetExecutor::begin_execution(unsigned long generation,
				     std::uint64_t guest_pc)
{
	preempted_ = false;
	auto *context = execution_context(generation);
	return context != nullptr &&
		darwin_uml_static_vm_execution_context_begin(context, guest_pc) == 0;
}

void GadgetExecutor::end_execution() noexcept
{
	if (current_cpu_ != nullptr) {
		/* A miss is a pointer into this executor's cache, not architectural
		 * state. Learning the target can allocate after the guest branch has
		 * retired; discard the marker even when that postamble throws. */
		current_cpu_->gadget_indirect_miss = nullptr;
		arm64::restore_native_fp_environment(*current_cpu_);
	}
	current_cpu_ = nullptr;
	if (execution_context_ != nullptr)
		(void)darwin_uml_static_vm_execution_context_end(execution_context_);
}

void GadgetExecutor::select_cpu(CpuState &cpu) noexcept
{
	if (current_cpu_ == &cpu)
		return;
	if (current_cpu_ != nullptr) {
		current_cpu_->gadget_indirect_miss = nullptr;
		arm64::restore_native_fp_environment(*current_cpu_);
	}
	/* The marker is executor-private scratch. Never inherit it across CPU or
	 * execution reuse; end_execution() resets current_cpu_ at every exit. */
	cpu.gadget_indirect_miss = nullptr;
	current_cpu_ = &cpu;
}

void *GadgetExecutor::execution_context(unsigned long generation)
{
	if (execution_context_ != nullptr && execution_generation_ == generation)
		return execution_context_;
	darwin_uml_static_vm_execution_context_destroy(execution_context_);
	execution_context_ =
		darwin_uml_static_vm_execution_context_create(generation);
	execution_generation_ = execution_context_ != nullptr ? generation : 0;
	return execution_context_;
}

void GadgetExecutor::prepare_cache(
	unsigned long generation, std::uint64_t changes)
{
	if (cache_generation_ == generation && cache_changes_ == changes)
		return;
#if UML_GADGET_EXACT_CACHE_VALIDATION
	/* Switching address space no longer empties the cache: slots, links and
	 * indirect sites all carry the address space they describe, so the
	 * translations of the one being left stay resident for its return. */
	programs_->prepare(generation, changes);
#else
	if (cache_generation_ != generation || cache_changes_ != changes)
		programs_->invalidate();
	programs_->prepare(generation, changes);
#endif
	cache_generation_ = generation;
	cache_changes_ = changes;
}

std::optional<GadgetExecutor::CachedResult> GadgetExecutor::find(
	std::uint64_t pc, std::uint64_t changes, StaticGuestMemory &memory)
{
	const auto *found = programs_->find(
		pc, changes, mapping_changes_, memory, execution_context_,
		cache_diagnostics_);
	if (found == nullptr)
		return std::nullopt;
	return std::cref(*found);
}

/* Produce a translation the cache did not hold.
 *
 * Asking the shared store first costs one hash of the fetched window and one
 * uncontended shard lookup, against a full decode of up to sixty-four
 * instructions when it fails -- and on this guest it succeeds far more often
 * than it fails, because most of what an executor is about to translate some
 * other executor has already translated. It is not worth asking when no other
 * executor exists to have answered.
 */
const BuildResult &GadgetExecutor::translate(
	std::uint64_t pc, std::span<const std::uint32_t> window)
{
	++cache_diagnostics_.translations;
	if (!arm64::gadgets::SharedProgramStore::sharing_worthwhile())
		return build(pc, window, nullptr);
	const auto key = arm64::gadgets::SharedKey::of(pc, window, profile_);
	if (const auto *adopted = programs_->adopt(key, cache_changes_);
	    adopted != nullptr) {
		++cache_diagnostics_.adoptions;
		return *adopted;
	}
	return build(pc, window, &key);
}

const BuildResult &GadgetExecutor::build(
	std::uint64_t pc, std::span<const std::uint32_t> window,
	const arm64::gadgets::SharedKey *key)
{
	arm64::gadgets::LinkRelocations relocations;
	auto decoded = arm64::gadgets::Builder{}.build(
		pc, window,
		[this](std::uint64_t target) {
			return programs_->link(target);
		},
		programs_->program_storage(),
		[this](std::uint64_t source_pc) {
			return programs_->indirect_link(source_pc);
		},
		build_options_, &relocations);
	if (key != nullptr)
		arm64::gadgets::SharedProgramStore::instance().publish(
			*key, decoded, relocations);
	return store(pc, std::move(decoded), window);
}

const BuildResult &GadgetExecutor::store(
	std::uint64_t pc, BuildResult result,
	std::span<const std::uint32_t> guest_words)
{
	return programs_->store(
		pc, std::move(result), guest_words, cache_changes_);
}

::uml::cpu::EngineEvent GadgetExecutor::run_block(
	arm64::CpuState &cpu, StaticGuestMemory &memory, unsigned long generation,
	std::size_t &executed)
{
	executed = 0;
	floating_point_syndrome_ = 0;
	select_cpu(cpu);
	auto *context = execution_context(generation);
	if (context == nullptr)
		return ::uml::cpu::EngineEvent::fatal;
	std::array<std::uint32_t, block_instruction_count> instructions;
	const auto sequences =
		darwin_uml_static_vm_execution_context_sequences(context);
	preempted_ = sequences.execution_interrupt != 0;
	if (preempted_)
		return ::uml::cpu::EngineEvent::interrupt;
	if (mapping_generation_ != generation ||
	    mapping_changes_ != sequences.mapping_changes) {
		memory.prepare(generation, sequences.mapping_changes);
		tlb_.prepare(generation, sequences.mapping_changes);
		mapping_generation_ = generation;
		mapping_changes_ = sequences.mapping_changes;
	}
	const auto guest_fpcr = cpu.fp_loaded ? cpu.fpcr : cpu.deferred_fpcr;
	if ((guest_fpcr &
	     arm64::detail::fpcr_exception_enable_mask) != 0U)
		return run_trap_enabled_instruction(cpu, memory, executed);
	prepare_cache(generation, sequences.code_changes);
	auto selected = find(cpu.pc, sequences.code_changes, memory);
	if (!selected) {
		const auto count = fetch_instruction_window(
			memory, cpu.pc, instructions);
		if (count == 0) {
			const auto fault = memory.last_fault();
			tlb_.fault_address = fault.address;
			tlb_.fault_access = ::uml::memory::translated_access(
				fault.access);
			return ::uml::cpu::EngineEvent::memory_fault;
		}
		const auto window = std::span{instructions}.first(count);
		selected = std::cref(translate(cpu.pc, window));
	}
	const auto &built = selected->get();
	if (built.program.instruction_count() == 0) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}
	const auto event =
		arm64::gadgets::execute_preserving_native_fp_environment(
			built.program, cpu, tlb_, executed);
	const bool fpcr_enable_exit =
		event == ::uml::cpu::EngineEvent::continued &&
		(effective_fpcr(cpu) &
		 arm64::detail::fpcr_exception_enable_mask) != 0U;
	const bool count_exit =
		terminal_does_not_retire(event) || fpcr_enable_exit;
	if (count_exit) {
		const auto *current_program = &built.program;
		const auto *exit_stream = cpu.gadget_indirect_miss;
		if (!program_contains_stream(*current_program, exit_stream)) {
			if (const auto *cached =
				    programs_->find_stream(exit_stream);
			    cached != nullptr)
				current_program = &cached->program;
		}
		if (fpcr_enable_exit)
			correct_fpcr_enable_instruction_count(
				*current_program, event, cpu, executed);
		else
			correct_terminal_instruction_count(
				*current_program, event, cpu.pc, executed);
		cpu.gadget_indirect_miss = nullptr;
	}
	if (cpu.gadget_indirect_miss != nullptr &&
	    (cpu.pc & (alignof(std::uint32_t) - 1U)) == 0U) {
		programs_->learn_indirect(
			*static_cast<arm64::gadgets::IndirectLink *>(
				cpu.gadget_indirect_miss),
			cpu.pc);
	}
	cpu.gadget_indirect_miss = nullptr;
	return event;
}

::uml::cpu::EngineEvent GadgetExecutor::run_trap_enabled_instruction(
	arm64::CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed)
{
	/* Guest trap enables are intentionally a cold execution mode.  Limiting
	 * every translated run to one instruction guarantees that an integer or
	 * memory instruction cannot carry execution past a later FP/SIMD
	 * data-processing instruction in the same cached block.
	 */
	cpu.gadget_indirect_miss = nullptr;
	arm64::gadgets::restore_native_fp_environment(cpu);
	std::uint32_t instruction = 0;
	auto encoded = std::as_writable_bytes(std::span{&instruction, 1});
	if (!memory.fetch(cpu.pc, encoded)) {
		const auto fault = memory.last_fault();
		tlb_.fault_address = fault.address;
		tlb_.fault_access = ::uml::memory::translated_access(fault.access);
		return ::uml::cpu::EngineEvent::memory_fault;
	}

	auto built = arm64::gadgets::Builder{}.build(
		cpu.pc, std::span{&instruction, 1}, {},
		std::pmr::get_default_resource(), {}, build_options_);
	if (!built.complete || built.program.instruction_count() != 1) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}

	const bool transactional =
		arm64::detail::needs_floating_exception_transaction(
			cpu.fp_loaded ? cpu.fpcr : cpu.deferred_fpcr,
			instruction);
	if (!transactional) {
		const auto event = arm64::gadgets::execute(
			built.program, cpu, tlb_, executed);
		correct_terminal_instruction_count(
			built.program, event, cpu.pc, executed);
		cpu.gadget_indirect_miss = nullptr;
		/* A one-instruction fallthrough ends in Builder's synthetic
		 * interrupt gadget.  It is an internal cold-mode boundary, not a
		 * guest-visible fairness interrupt.
		 */
		return event == ::uml::cpu::EngineEvent::interrupt ?
			::uml::cpu::EngineEvent::continued : event;
	}

	cpu.access_fp_state();
	const auto original_fpsr = cpu.fpsr;
	arm64::CpuState isolated = cpu;
	isolated.fpsr &=
		~arm64::detail::fpsr_exception_cumulative_mask;

	std::size_t isolated_executed = 0;
	const auto event = arm64::gadgets::execute(
		built.program, isolated, tlb_, isolated_executed);
	if (event != ::uml::cpu::EngineEvent::interrupt &&
	    event != ::uml::cpu::EngineEvent::continued) {
		/* Reserved allocations and other non-executing events retain the
		 * live architectural state, just like the strict interpreter.
		 */
		correct_terminal_instruction_count(
			built.program, event, isolated.pc, isolated_executed);
		isolated.gadget_indirect_miss = nullptr;
		executed = isolated_executed;
		return event;
	}

	const auto decision = arm64::detail::decide_floating_exception(
		cpu.fpcr, original_fpsr, isolated.fpsr);
	if (decision.traps()) {
		cpu.fpsr = decision.committed_fpsr;
		floating_point_syndrome_ = decision.syndrome;
		executed = 0;
		return ::uml::cpu::EngineEvent::floating_point_trap;
	}

	/* The scratch execution began with cumulative flags clear.  Preserve
	 * every other FPSR field produced by the gadget while restoring the
	 * pre-existing cumulative state before committing the transaction.
	 */
	isolated.fpsr =
		(isolated.fpsr &
		 ~arm64::detail::fpsr_exception_cumulative_mask) |
		(decision.committed_fpsr &
		 arm64::detail::fpsr_exception_cumulative_mask);
	cpu = isolated;
	executed = isolated_executed;
	return ::uml::cpu::EngineEvent::continued;
}

std::uint64_t GadgetExecutor::fault_address() const noexcept
{
	return tlb_.fault_address;
}

::uml::memory::Access GadgetExecutor::fault_access() const noexcept
{
	return tlb_.fault_access;
}

std::uint32_t GadgetExecutor::floating_point_syndrome() const noexcept
{
	return floating_point_syndrome_;
}

} // namespace uml::cpu::arm64::gadgets
