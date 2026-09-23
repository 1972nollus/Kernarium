// SPDX-License-Identifier: GPL-2.0-or-later
#include "executor.hpp"

#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/gadget_cache_geometry.hpp"
#include "cpu/gadget_record_table.hpp"
#include "shared_programs.hpp"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <bit>
#if UML_GADGET_CACHE_CENSUS
#include <cstdio>
#include <cstdlib>
#endif
#include <cstring>
#include <iterator>
#include <memory>
#include <memory_resource>
#if UML_GADGET_CACHE_CENSUS
#include <mutex>
#endif
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
/* Translating a direct call's callee inline behind it, so the call and its
 * return stop being block boundaries. Calls end 48.9% of blocks once
 * conditional fall-throughs are inlined, and a return is an indirect branch
 * through a two-entry link that any callee reached from more than two sites
 * thrashes. See G163. */
#ifndef UML_GADGET_INLINE_CALLS
#define UML_GADGET_INLINE_CALLS 1
#endif
constexpr bool inline_calls_enabled = UML_GADGET_INLINE_CALLS != 0;
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

	const auto position = program_position(program, fault_pc);
	if (!position || *position >= program.instruction_count())
		return;
	const auto unretired = program.instruction_count() - *position;
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

	/* The exit is the fallthrough of the FPCR write, so it is either inside
	 * the program or one past its end. Only the final run can end a program:
	 * an earlier run ends at the call whose callee was inlined after it, and
	 * a call is never an FPCR write.
	 */
	std::size_t retired = 0;
	if (const auto position = program_position(program, cpu.pc); position) {
		retired = *position;
	} else {
		const auto &last = program.runs().back();
		if (cpu.pc != last.address + std::uint64_t{last.instructions} *
				sizeof(std::uint32_t))
			return;
		retired = program.instruction_count();
	}
	if (retired == 0 || retired > program.instruction_count())
		return;
	const auto unretired = program.instruction_count() - retired;
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

constexpr std::size_t uml_gadget_cache_slots = ::uml::cpu::gadget_cache::slots;
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
	::uml::cpu::gadget_cache::resident_generations;

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

/* Storage for stored translations.
 *
 * A translation is one allocation: its body, the stream words behind it, and
 * the guest words it is validated against behind those, sized exactly. The
 * cache holds tens of thousands of them per executor, so a rounding allocator
 * costs more than the streams do: a pmr pool asked its upstream for 204 MiB to
 * hold 64 MiB of programs on a measured six-vCPU guest, between its own size
 * classes and the capacity a stream reached by doubling. Sixteen-byte classes
 * over bump-allocated chunks hold the same programs in 65 MiB.
 *
 * Freed bodies return to the class they came from, so retranslating a block
 * reuses its predecessor's bytes; chunks are returned only when the cache is
 * destroyed, which is what keeps retiring an address space proportional to
 * what it owned rather than to a serialised allocator walk. See G167.
 *
 * The arena also answers which live body holds an address. Each chunk keeps
 * one bit per granule, set at the first granule of every live block, so the
 * block holding an address is the nearest set bit at or below it. The cache
 * asks this when a terminal exit lands in a linked successor, which a
 * measured guest did about once per hundred and twenty stores; the index by
 * stream address that this replaces was maintained at every one of those
 * stores, five set installs per program. See G203.
 */
class BodyArena final {
public:
	explicit BodyArena(std::pmr::memory_resource *upstream) noexcept
		: upstream_{upstream}
	{}

	BodyArena(const BodyArena &) = delete;
	BodyArena &operator=(const BodyArena &) = delete;

	~BodyArena()
	{
		for (const auto &chunk : chunks_)
			upstream_->deallocate(chunk.base, chunk.bytes,
				alignof(std::max_align_t));
	}

	[[nodiscard]] void *allocate(std::size_t bytes)
	{
		const auto size = rounded(bytes);
		if (size > largest_class) {
			/* Too big for a class to recycle, so it is not carved
			 * out of a chunk either; it is freed on its own. */
			auto *block = static_cast<std::byte *>(
				upstream_->allocate(size,
					alignof(std::max_align_t)));
			direct_.push_back({block, size});
			return block;
		}
		if (auto *block = free_[size / granule]; block != nullptr) {
			free_[size / granule] = next_of(block);
			mark(block, true);
			return block;
		}
		if (size > limit_ - used_) {
			const auto wanted = std::max(size, chunk_bytes);
			auto starts = std::make_unique<std::uint64_t[]>(
				map_words(wanted));
			auto *base = static_cast<std::byte *>(
				upstream_->allocate(wanted,
					alignof(std::max_align_t)));
			/* Kept in address order, which is what owner() searches. */
			const auto inserted = chunks_.insert(
				std::upper_bound(chunks_.begin(), chunks_.end(),
					base, before_chunk),
				Chunk{base, wanted, std::move(starts)});
			/* The tail of the chunk being left behind is at most one
			 * body, and one partly used chunk is the arena's whole
			 * slack. */
			current_ = base;
			current_starts_ = inserted->starts.get();
			used_ = 0;
			limit_ = wanted;
		}
		auto *block = current_ + used_;
		current_starts_[used_ / granule / 64] |=
			std::uint64_t{1} << (used_ / granule % 64);
		used_ += size;
		return block;
	}

	void deallocate(void *block, std::size_t bytes) noexcept
	{
		const auto size = rounded(bytes);
		auto *freed = static_cast<std::byte *>(block);
		if (size > largest_class) {
			const auto held = std::find_if(direct_.begin(),
				direct_.end(), [freed](const Direct &direct) {
					return direct.base == freed;
				});
			if (held != direct_.end())
				direct_.erase(held);
			upstream_->deallocate(block, size,
				alignof(std::max_align_t));
			return;
		}
		mark(freed, false);
		next_of(freed) = free_[size / granule];
		free_[size / granule] = freed;
	}

	/* The live block holding an address, or nullptr. */
	[[nodiscard]] void *owner(const void *address) const noexcept
	{
		const auto *at = static_cast<const std::byte *>(address);
		if (const auto *chunk = chunk_of(at); chunk != nullptr) {
			const auto index =
				static_cast<std::size_t>(at - chunk->base) / granule;
			/* Nothing in a chunk spans more than the largest class,
			 * so a start any further below is another block's. */
			const auto floor = index >= largest_class / granule ?
				(index - largest_class / granule) / 64 : 0;
			auto word = index / 64;
			auto bits = chunk->starts[word] &
				(~std::uint64_t{0} >> (63 - index % 64));
			for (;;) {
				if (bits != 0)
					return chunk->base +
						(word * 64 + 63 -
							static_cast<std::size_t>(
								std::countl_zero(bits))) *
						granule;
				if (word == floor)
					return nullptr;
				bits = chunk->starts[--word];
			}
		}
		for (const auto &direct : direct_)
			if (at >= direct.base && at < direct.base + direct.bytes)
				return direct.base;
		return nullptr;
	}

	/* What the arena holds, which is what the cache costs. */
	[[nodiscard]] std::size_t bytes() const noexcept
	{
		std::size_t total = 0;
		for (const auto &direct : direct_)
			total += direct.bytes;
		for (const auto &chunk : chunks_)
			total += chunk.bytes;
		return total;
	}

	/* The start maps, which are what owner() costs. */
	[[nodiscard]] std::size_t index_bytes() const noexcept
	{
		std::size_t total = 0;
		for (const auto &chunk : chunks_)
			total += map_words(chunk.bytes) * sizeof(std::uint64_t);
		return total;
	}

private:
	/* Eight bytes would fit the free-list link, but sixteen keeps every
	 * body's stream tail aligned for a pair access and still rounds a mean
	 * body by eight bytes. */
	static constexpr std::size_t granule = 16;
	/* Up to 16 KiB in classes, which a 64-instruction block reaches only at
	 * 32 stream words per guest instruction. */
	static constexpr std::size_t classes = 1024;
	static constexpr std::size_t largest_class = (classes - 1) * granule;
	static constexpr std::size_t chunk_bytes = std::size_t{256} << 10;

	[[nodiscard]] static std::size_t rounded(std::size_t bytes) noexcept
	{
		return (std::max(bytes, granule) + granule - 1) & ~(granule - 1);
	}

	[[nodiscard]] static std::byte *&next_of(std::byte *block) noexcept
	{
		return *reinterpret_cast<std::byte **>(block);
	}

	/* Words in the start map of a chunk of this many bytes. */
	[[nodiscard]] static constexpr std::size_t map_words(
		std::size_t bytes) noexcept
	{
		return (bytes / granule + 63) / 64;
	}

	struct Chunk final {
		std::byte *base;
		std::size_t bytes;
		/* One bit per granule, set at the first granule of a live block. */
		std::unique_ptr<std::uint64_t[]> starts;
	};

	/* A block above the largest class, held outside the chunks. */
	struct Direct final {
		std::byte *base;
		std::size_t bytes;
	};

	[[nodiscard]] static bool before_chunk(
		const std::byte *address, const Chunk &chunk) noexcept
	{
		return address < chunk.base;
	}

	[[nodiscard]] const Chunk *chunk_of(
		const std::byte *address) const noexcept
	{
		const auto after = std::upper_bound(
			chunks_.begin(), chunks_.end(), address, before_chunk);
		if (after == chunks_.begin())
			return nullptr;
		const auto &chunk = *std::prev(after);
		return address < chunk.base + chunk.bytes ? &chunk : nullptr;
	}

	void mark(const std::byte *block, bool live) noexcept
	{
		const auto *chunk = chunk_of(block);
		const auto index =
			static_cast<std::size_t>(block - chunk->base) / granule;
		const auto bit = std::uint64_t{1} << (index % 64);
		if (live)
			chunk->starts[index / 64] |= bit;
		else
			chunk->starts[index / 64] &= ~bit;
	}

	std::pmr::memory_resource *upstream_;
	/* In address order. */
	std::vector<Chunk> chunks_;
	std::vector<Direct> direct_;
	std::byte *current_{};
	std::uint64_t *current_starts_{};
	std::size_t used_{};
	std::size_t limit_{};
	std::array<std::byte *, classes> free_{};
};

class GadgetExecutor::ProgramCache final {
	/* Named by member declarations below and defined with the rest of the
	 * private state. */
	struct LinkRecord;
	struct Generation;

public:
	ProgramCache()
		: bodies_{&counted_},
		  build_scratch_{scratch_.data(), scratch_.size()}
	{}

	~ProgramCache()
	{
		for (auto &slab : slabs_) {
			if (!slab)
				continue;
			for (auto &slot : slab->slots) {
				if (slot.body == nullptr)
					continue;
				delete_body(slot.body);
				slot.body = nullptr;
			}
		}
	}

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
		std::size_t links = 0;
		std::size_t indirect = 0;
		for (const auto &[generation, state] : generations_) {
			links += state.links.bytes() +
				state.live_links.capacity() *
					sizeof(LinkRecord *) +
				state.slots.capacity() * sizeof(std::uint32_t);
			indirect += state.indirect.bytes() +
				state.live_indirect.capacity() * sizeof(void *);
		}
		return {
			.stream_index = bodies_.index_bytes(),
			.slabs = slabs,
			.streams = bodies_.bytes(),
			.links = links,
			.indirect = indirect,
		};
	}

#if UML_GADGET_CACHE_CENSUS
	/* Write what this cache holds, so duplication can be counted instead of
	 * estimated.
	 *
	 * A translation is identified by the guest words it consumed, not by the
	 * stream it produced: two executors translating the same code emit the
	 * same words but embed pointers to their own link records, so only the
	 * source identifies the work. The stream length goes out with it because
	 * it decides what a body costs.
	 */
	void census(std::FILE *out) const noexcept
	{
		const auto parts = footprint();
		/* How often the stream lookup was asked, against how often a
		 * store would have had to maintain an index for it. */
		std::fprintf(out,
			"E %p generations=%zu occupancy=%zu stream_index=%zu "
			"slabs=%zu streams=%zu links=%zu indirect=%zu total=%zu "
			"stores=%llu stream_lookups=%llu\n",
			static_cast<const void *>(this), resident_generations(),
			occupancy(), parts.stream_index, parts.slabs,
			parts.streams, parts.links, parts.indirect,
			parts.total(),
			static_cast<unsigned long long>(stores_),
			static_cast<unsigned long long>(stream_lookups_));
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots) {
				if (slot.epoch != epoch_ || slot.body == nullptr)
					continue;
				const auto &program = slot.body->result.program;
				std::uint64_t digest = 0xcbf29ce484222325ULL;
				std::size_t sources = 0;
#if UML_GADGET_EXACT_CACHE_VALIDATION
				sources = slot.body->source_word_count;
				const auto *words = slot.body->source_words();
				for (std::size_t index = 0; index < sources;
				     ++index)
					digest = (digest ^ words[index]) *
						0x100000001b3ULL;
#endif
				std::fprintf(out,
					"P %lu 0x%llx %zu %zu %zu 0x%llx\n",
					slot.generation,
					static_cast<unsigned long long>(slot.pc),
					program.instruction_count(),
					program.words().size(), sources,
					static_cast<unsigned long long>(digest));
			}
		}
	}
#endif

	/* How many address spaces currently hold translations here. With one
	 * shared pool this is an outcome of the workload, not a build option. */
	[[nodiscard]] std::size_t resident_generations() const noexcept
	{
		return generations_.size();
	}

	[[nodiscard]] std::size_t occupancy() const noexcept
	{
		std::size_t occupied = 0;
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots)
				if (slot.epoch == epoch_ && slot.body != nullptr)
					++occupied;
		}
		return occupied;
	}

	void invalidate() noexcept
	{
		inlined_callees_.clear();
		resident_.clear();
		generations_.clear();
		current_ = nullptr;
		++epoch_;
		if (epoch_ != 0)
			return;
		for (auto &slab : slabs_)
			if (slab)
				for (auto &slot : slab->slots)
					slot.epoch = 0;
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
	void invalidate_links(Generation &state) noexcept
	{
		for (auto *record : state.live_links) {
			record->reset();
			record->listed = false;
		}
		state.live_links.clear();
		if (state.live_indirect.empty())
			return;
		/* The reset cells name this address space's own empty record, so
		 * a site never points at another's link state. */
		const auto *empty_link = &link_record(state, empty_pc).link;
		for (auto *site : state.live_indirect) {
			for (auto &entry : site->entries) {
				entry.guest_pc = empty_pc;
				entry.link = empty_link;
			}
			site->next_replacement = 0;
		}
		state.live_indirect.clear();
	}

	/* Bind the cache to an address space and answer for whatever changed in
	 * it while it was away. Each address space carries its own mark, so a
	 * switch back to one whose code never changed costs nothing. */
	void prepare(unsigned long generation, std::uint64_t changes)
	{
		current_generation_ = generation;
		auto [entry, inserted] = generations_.try_emplace(generation);
		current_ = &entry->second;
		touch(generation);
		if (inserted) {
			current_->changes = changes;
			while (resident_.size() > uml_gadget_resident_generations)
				retire(resident_.front());
			return;
		}
		if (current_->changes == changes)
			return;
		invalidate_links(*current_);
		current_->changes = changes;
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
			    slot.generation != current_generation_ ||
			    slot.body == nullptr)
				continue;
#if UML_GADGET_EXACT_CACHE_VALIDATION
			if (slot.validated_changes == changes) {
				return &slot.body->result;
			}
			++diagnostics.validation_attempts;
			if (!validate(slot, changes, mapping_changes, memory,
				    execution_context, diagnostics)) {
				return nullptr;
			}
			activate(pc, slot);
			return &slot.body->result;
#else
			(void)changes;
			(void)mapping_changes;
			(void)memory;
			(void)execution_context;
			(void)diagnostics;
			return &slot.body->result;
#endif
		}
		return nullptr;
	}

	/* The translation whose stream holds an address.
	 *
	 * A terminal exit inside a linked successor reports the successor's
	 * stream, and the retirement correction needs that successor's program
	 * rather than the entered block's. The arena knows which live body
	 * holds any address, and a stream pointer the engine has just produced
	 * always lies in a live body: links only ever name current
	 * translations, and nothing is stored or released between the exit and
	 * this lookup. The answer is exact, with no index to fall out of.
	 */
	[[nodiscard]] const BuildResult *find_stream(
		const void *stream) const noexcept
	{
		if (stream == nullptr)
			return nullptr;
#if UML_GADGET_CACHE_CENSUS
		++stream_lookups_;
#endif
		const auto *body = static_cast<const Body *>(
			bodies_.owner(stream));
		if (body == nullptr ||
		    !program_contains_stream(body->result.program, stream))
			return nullptr;
		return &body->result;
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
			current().indirect.try_emplace(source_pc, source_pc);
		if (inserted) {
			const auto *empty_link = link(empty_pc);
			for (auto &entry : created->site.entries) {
				entry.guest_pc = empty_pc;
				entry.link = empty_link;
			}
		}
		return &created->site;
	}

	void learn_indirect(arm64::gadgets::IndirectLink &site,
			    std::uint64_t target_pc)
	{
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* Distinct targets seen per site, and misses per site, so the
		 * dump can say how many cells a site would have needed. */
		++site_misses_[&site][target_pc];
#endif
		const auto *target = link(target_pc);
		/* Training is the only thing that moves a site off its empty
		 * cells, and empty_pc is unreachable as a guest target, so a
		 * site still holding two empty cells is untrained since its last
		 * reset -- which makes this the transition that enrols it. */
		if (std::all_of(site.entries.begin(), site.entries.end(),
			    [](const auto &entry) {
				    return entry.guest_pc == empty_pc;
			    }))
			current().live_indirect.push_back(&site);
		for (auto &entry : site.entries) {
			if (entry.guest_pc != target_pc)
				continue;
			entry.link = target;
			return;
		}
		auto &entry = site.entries[site.next_replacement %
			arm64::gadgets::indirect_link_entries];
		entry.guest_pc = target_pc;
		entry.link = target;
		site.next_replacement = static_cast<std::uint8_t>(
			(site.next_replacement + 1U) %
			arm64::gadgets::indirect_link_entries);
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
		std::pmr::vector<std::uintptr_t> words{program_storage()};
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
		/* Record the slot against the address space taking it, so its
		 * retirement walks what it owns instead of the whole cache. */
		auto &state = current();
		if (state.slots_complete) {
			if (state.slots.size() < slot_count) {
				state.slots.push_back(static_cast<std::uint32_t>(
					location.slab * slab_slots +
					location.offset + selected));
			} else {
				state.slots_complete = false;
				state.slots.clear();
				state.slots.shrink_to_fit();
			}
		}
		/* The words a validating find() will compare against guest
		 * memory: the program's runs concatenated, plus the word a
		 * stopped build read without retiring. */
		std::span<const std::uint32_t> validation;
#if UML_GADGET_EXACT_CACHE_VALIDATION
		const auto validation_words =
			result.program.instruction_count() +
			static_cast<std::size_t>(!result.complete);
		if (validation_words != 0 &&
		    validation_words <= block_instruction_count &&
		    validation_words <= guest_words.size())
			validation = guest_words.first(validation_words);
#endif
		auto *replacement_body = new_body(result, validation);
		/* An epoch this slot no longer belongs to means its former owner
		 * is already gone, so only a current occupant has a link record
		 * left to reset -- but either way the body it holds is this
		 * cache's to release. */
		if (slot.body != nullptr) {
			if (slot.epoch == epoch_)
				deactivate(slot.generation, slot.pc,
					slot.body->result.program);
			delete_body(slot.body);
			slot.body = nullptr;
		}
		/* The memoised binding describes whoever held the slot until
		 * now, and a store may hand the slot to a different address
		 * space at the same guest PC -- where every test activate()
		 * makes still passes. Left in place it would publish this
		 * translation into the *previous* address space's record,
		 * branching its code into this one and leaving the slot naming a
		 * record that retiring that address space destroys. Taking a
		 * slot therefore always resolves the binding again; ordinary
		 * reactivation of an unchanged owner keeps the memo.
		 */
		slot.link = nullptr;
		slot.link_epoch = 0;
		slot.body = replacement_body;
		slot.pc = pc;
		slot.generation = current_generation_;
		slot.epoch = epoch_;
#if UML_GADGET_EXACT_CACHE_VALIDATION
		slot.validated_changes = changes;
#else
		(void)guest_words;
		(void)changes;
#endif
#if UML_GADGET_CACHE_CENSUS
		++stores_;
#endif
		activate(pc, slot);
		return slot.body->result;
	}

	/* Whether a callee may be translated inline here.
	 *
	 * A callee reached from one site costs nothing to inline; one reached
	 * from many would be translated once per site. Admitting each callee
	 * once caps that at a single extra copy without needing to know how many
	 * sites exist, which translation cannot see. */
	[[nodiscard]] bool admit_inlined_call(std::uint64_t callee)
	{
		if (inlined_callees_.size() >= maximum_inlined_callees)
			return false;
		/* Only the first site to reach a callee inlines it. Admitting
		 * it again on a later site is what "inline everywhere" means,
		 * and duplicates the callee once per site. */
		return inlined_callees_.emplace(callee, epoch_).second;
	}

	/* Where a build assembles its stream.
	 *
	 * store() copies the finished words into the body it allocates, so the
	 * scratch is rewound at the start of every build and a translation
	 * costs no allocation until it is stored. Only one build is ever in
	 * flight on an executor.
	 */
	[[nodiscard]] std::pmr::memory_resource *program_storage() noexcept
	{
		build_scratch_.release();
		return &build_scratch_;
	}

private:
	static constexpr std::size_t ways = ::uml::cpu::gadget_cache::ways;
	static constexpr std::size_t slot_count = uml_gadget_cache_slots;
	static constexpr std::size_t set_count = slot_count / ways;
	static constexpr std::size_t slab_slots = ::uml::cpu::gadget_cache::slab_slots;
	static constexpr std::size_t slab_count = slot_count / slab_slots;
	/* The unreachable guest PC an untrained indirect cell names; guest
	 * targets are admitted only word-aligned, so none can collide. */
	static constexpr std::uint64_t empty_pc = ~std::uint64_t{};
	static_assert((slot_count & (slot_count - 1)) == 0,
		"cache slot count must be a power of two");
	static_assert(slot_count >= slab_slots * 4,
		"cache must hold at least four slabs");

	/* One translation, allocated apart from the slot that indexes it.
	 *
	 * A set way exists whether or not it holds a translation, so every byte
	 * kept inline is paid for by the whole cache. With the program, its
	 * stream and its validation words moved into one separately allocated
	 * body, an empty way costs a tag rather than a full BuildResult, and the
	 * words a spilling stream used to allocate on their own now share the
	 * body's allocation.
	 */
	struct Body final {
		BuildResult result;
		/* What the arena must be given back, so the body carries it
		 * rather than the slot that indexes it. */
		std::uint32_t bytes{};
		std::uint32_t word_count{};
#if UML_GADGET_EXACT_CACHE_VALIDATION
		std::uint32_t source_word_count{};
#endif

		/* The stream follows the body, and the guest words it is
		 * validated against follow the stream. */
		[[nodiscard]] std::uintptr_t *words() noexcept
		{
			return reinterpret_cast<std::uintptr_t *>(this + 1);
		}
#if UML_GADGET_EXACT_CACHE_VALIDATION
		[[nodiscard]] std::uint32_t *source_words() noexcept
		{
			return reinterpret_cast<std::uint32_t *>(
				words() + word_count);
		}
		[[nodiscard]] const std::uint32_t *source_words() const noexcept
		{
			return reinterpret_cast<const std::uint32_t *>(
				reinterpret_cast<const std::uintptr_t *>(
					this + 1) + word_count);
		}
#endif
	};
	static_assert(alignof(Body) == alignof(std::uintptr_t) &&
		sizeof(Body) % alignof(std::uintptr_t) == 0,
		"the stream tail follows the body at pointer alignment");

	struct Slot final {
		Body *body{};
		/* The address space this translation belongs to. A guest virtual
		 * address only means something inside one, so a slot is a match
		 * only when the generation matches too; that is what lets
		 * translations for several address spaces share one cache
		 * instead of being wiped at every switch.
		 */
		unsigned long generation{};
		std::uint64_t pc{};
		std::uint64_t epoch{};
		/* Reactivation resolves the same link record every time, so the
		 * node address is memoised here rather than hashed again on each
		 * pass. The owning address space's link map is node-based, so the
		 * address is stable until invalidate() clears it, and that also
		 * advances epoch_, which is what link_epoch is compared against.
		 * Retiring the address space clears this slot's epoch in the same
		 * pass that destroys the map.
		 *
		 * The memo is only ever valid for the address space named by
		 * `generation`, and nothing about the record itself says which
		 * one that is: the guest PC it carries is equal across all of
		 * them. Ordinary slot replacement does not keep the owner, so
		 * store() resets this pair rather than letting activate() decide
		 * from the record alone.
		 */
		LinkRecord *link{};
		std::uint64_t link_epoch{};
#if UML_GADGET_EXACT_CACHE_VALIDATION
		std::uint64_t validated_changes{};
#endif
	};

	/* The cache commits a slot per way whether or not it is occupied, so
	 * the tag size multiplies by slot_count: at 65,536 slots every eight
	 * bytes here is half a mebibyte per executor. Anything that belongs to
	 * one translation belongs in its Body. */
	static_assert(sizeof(Slot) <= 56,
		"cache slots index translations; they do not hold them");

	struct Slab final {
		std::array<Slot, slab_slots> slots;
	};

	struct Location final {
		std::size_t set;
		std::size_t slab;
		std::size_t offset;
	};

	/* A link names a guest PC, which is only meaningful inside one address
	 * space; keying by PC alone would let a block in one branch straight
	 * into another's translation. Each address space therefore owns its own
	 * map, which also keeps the key a single word. */
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
		/* Set while this record sits in its address space's live list, so
		 * a code change resets the records that hold an activation
		 * instead of every record the translator ever named. */
		bool listed{};
	};

	struct LinkKey final {
		[[nodiscard]] std::uint64_t operator()(
			const LinkRecord &record) const noexcept
		{
			return record.link.guest_pc;
		}
	};

	/* An indirect site is named by the branch that owns it, which its own
	 * cells do not record. Translated streams point at the site, the first
	 * member. */
	struct IndirectRecord final {
		explicit IndirectRecord(std::uint64_t source) noexcept
			: source_pc{source}
		{}

		arm64::gadgets::IndirectLink site{};
		std::uint64_t source_pc;
	};

	struct IndirectKey final {
		[[nodiscard]] std::uint64_t operator()(
			const IndirectRecord &record) const noexcept
		{
			return record.source_pc;
		}
	};

	/* Everything that belongs to one address space.
	 *
	 * Retiring an address space is then dropping this, rather than filtering
	 * it out of structures the others share. That matters because retirement
	 * is not rare on a desktop guest: it happens on every first sight of a
	 * ninth address space, and the filtering version had to walk every slot
	 * in the cache and every link record of every address space to find the
	 * one being retired. See G167.
	 */
	struct Generation final {
		Generation() = default;
		Generation(const Generation &) = delete;
		Generation &operator=(const Generation &) = delete;
		Generation(Generation &&) = delete;
		Generation &operator=(Generation &&) = delete;

		std::uint64_t changes{};
		/* These grow for the address space's whole residency and are
		 * released only together. The records live in chunks, so retiring
		 * an address space is a few bulk releases rather than thousands of
		 * allocator frees across busy vCPU carriers, and each record stays
		 * where translated link cells name it. */
		gadget_cache::StableRecordTable<LinkRecord, LinkKey> links;
		gadget_cache::StableRecordTable<IndirectRecord, IndirectKey>
			indirect;
		/* The records and sites holding an activation. */
		std::vector<LinkRecord *> live_links;
		std::vector<arm64::gadgets::IndirectLink *> live_indirect;
		/* Slots this address space has stored into, as flat slot
		 * indices. It may name a slot another address space has since
		 * taken, and may name one twice, so retirement checks every
		 * entry; it can only *miss* a slot by having overflowed, which
		 * `slots_complete` records and retirement answers with the whole
		 * sweep.
		 */
		std::vector<std::uint32_t> slots;
		bool slots_complete{true};
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
	 * its indirect sites.
	 *
	 * The slots come from the list the address space kept while it was
	 * storing, so this is proportional to what it owned rather than to the
	 * size of the cache; the link records, indirect sites and live lists go
	 * with the Generation itself. No deactivate() is needed on the way out,
	 * because the records those calls would reset are about to be destroyed.
	 */
	void retire(unsigned long generation)
	{
		const auto owner = generations_.find(generation);
		if (owner != generations_.end() &&
		    owner->second.slots_complete) {
			for (const auto index : owner->second.slots)
				release(slot_at(index), generation);
		} else {
			for (auto &slab : slabs_) {
				if (!slab)
					continue;
				for (auto &slot : slab->slots)
					release(slot, generation);
			}
		}
		if (owner != generations_.end()) {
			if (current_ == &owner->second)
				current_ = nullptr;
			generations_.erase(owner);
		}
		const auto found = std::find(
			resident_.begin(), resident_.end(), generation);
		if (found != resident_.end())
			resident_.erase(found);
	}

	void release(Slot &slot, unsigned long generation) noexcept
	{
		if (slot.epoch != epoch_ || slot.generation != generation ||
		    slot.body == nullptr)
			return;
		delete_body(slot.body);
		slot.body = nullptr;
		slot.epoch = 0;
		slot.link = nullptr;
	}

	/* Store a freshly built translation as one exact allocation.
	 *
	 * The build's own words live in the scratch the next build reuses, so
	 * they are copied into the body's tail and the stored program is given
	 * those words to refer to rather than a second copy of them.
	 */
	[[nodiscard]] Body *new_body(
		const BuildResult &built, std::span<const std::uint32_t> validation)
	{
		const auto words = built.program.words();
		const auto bytes = sizeof(Body) +
			words.size() * sizeof(std::uintptr_t) +
			validation.size() * sizeof(std::uint32_t);
		auto *storage = static_cast<std::byte *>(bodies_.allocate(bytes));
		auto *tail = reinterpret_cast<std::uintptr_t *>(
			storage + sizeof(Body));
		std::copy(words.begin(), words.end(), tail);
		auto *body = new (storage) Body{
			.result = BuildResult{
				arm64::gadgets::Program{
					built.program.runs(),
					built.program.instruction_count(),
					arm64::gadgets::ProgramWords{
						arm64::gadgets::ProgramWords::
							Borrowed{tail,
								words.size()}},
					built.program.accesses_fp_state()},
				built.complete, built.stop_address,
				built.stop_instruction},
			.bytes = static_cast<std::uint32_t>(bytes),
			.word_count = static_cast<std::uint32_t>(words.size()),
#if UML_GADGET_EXACT_CACHE_VALIDATION
			.source_word_count =
				static_cast<std::uint32_t>(validation.size()),
#endif
		};
#if UML_GADGET_EXACT_CACHE_VALIDATION
		std::copy(validation.begin(), validation.end(),
			body->source_words());
#else
		(void)validation;
#endif
		return body;
	}

	void delete_body(Body *body) noexcept
	{
		const auto bytes = body->bytes;
		body->~Body();
		bodies_.deallocate(body, bytes);
	}

	[[nodiscard]] Slot &slot_at(std::uint32_t index) noexcept
	{
		auto &slab = slabs_[index / slab_slots];
		return slab->slots[index % slab_slots];
	}

	/* The address space translation is currently running in.
	 *
	 * prepare() sets this on every switch, but its caller may skip the call
	 * when nothing changed, and on the very first entry there is nothing to
	 * compare against -- so the binding is completed here rather than
	 * assumed. */
	[[nodiscard]] Generation &current()
	{
		if (current_ == nullptr)
			current_ = &generations_
					    .try_emplace(current_generation_)
					    .first->second;
		return *current_;
	}

	[[nodiscard]] LinkRecord &link_record(Generation &state, std::uint64_t pc)
	{
		return *state.links.try_emplace(pc, pc).first;
	}

	[[nodiscard]] LinkRecord &link_record(std::uint64_t pc)
	{
		return link_record(current(), pc);
	}

#if UML_GADGET_EXACT_CACHE_VALIDATION
	void discard(Slot &slot) noexcept
	{
		if (slot.body != nullptr) {
			deactivate(slot.generation, slot.pc,
				slot.body->result.program);
			delete_body(slot.body);
			slot.body = nullptr;
		}
		slot.epoch = 0;
		slot.validated_changes = 0;
	}

	[[nodiscard]] bool validate(
		Slot &slot, std::uint64_t changes, std::uint64_t mapping_changes,
		StaticGuestMemory &memory, void *execution_context,
		GadgetCacheDiagnostics &diagnostics)
	{
		if (slot.body == nullptr || slot.body->source_word_count == 0 ||
		    slot.body->source_word_count > block_instruction_count) {
			++diagnostics.validation_mismatches;
			discard(slot);
			return false;
		}

		/* The saved words are the program's runs concatenated in order,
		 * so each is compared against the addresses its own run covers.
		 * The word a stopped build read without retiring belongs to the
		 * last run, which is where the build stopped. */
		std::array<std::uint32_t, block_instruction_count> fetched;
		const auto &program = slot.body->result.program;
		const auto runs = program.runs();
		std::size_t saved = 0;
		for (std::size_t index = 0; index < runs.size(); ++index) {
			auto remaining = std::size_t{runs[index].instructions};
			if (index + 1 == runs.size())
				remaining += slot.body->source_word_count -
					program.instruction_count();
			std::size_t offset = 0;
			while (offset < remaining) {
				const auto address = runs[index].address +
					offset * sizeof(std::uint32_t);
				const auto page_remaining = guest_page_size -
					(address & (guest_page_size - 1));
				auto run = std::min<std::size_t>(
					remaining - offset,
					page_remaining / sizeof(std::uint32_t));
				if (run == 0)
					run = 1;
				auto words = std::span{fetched}.subspan(
					saved + offset, run);
				if (!memory.fetch(address,
						std::as_writable_bytes(words))) {
					++diagnostics.validation_faults;
					discard(slot);
					return false;
				}
				if (std::memcmp(words.data(),
						slot.body->source_words() +
							saved + offset,
						run * sizeof(std::uint32_t)) != 0) {
					++diagnostics.validation_mismatches;
					discard(slot);
					return false;
				}
				offset += run;
			}
			saved += remaining;
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
		const auto &result = slot.body->result;
		if (!result.complete || result.program.instruction_count() == 0)
			return;
		if (slot.link == nullptr || slot.link_epoch != epoch_ ||
		    slot.link->link.guest_pc != pc) {
			slot.link = &link_record(pc);
			slot.link_epoch = epoch_;
		}
		auto &record = *slot.link;
		if (!record.listed) {
			record.listed = true;
			current().live_links.push_back(&record);
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
		const auto owner = generations_.find(generation);
		if (owner == generations_.end())
			return;
		auto *found = owner->second.links.find(pc);
		if (found == nullptr)
			return;
		auto &record = *found;
		const auto *words = program.words().data();
		const bool direct = record.link.stream == words;
		const bool fp = record.link.stream == record.fp_stream.data() &&
			record.fp_stream[1] ==
				reinterpret_cast<std::uintptr_t>(words);
		if (direct || fp)
			record.reset();
	}

#if UML_GADGET_EXECUTION_HISTOGRAM
public:
	std::unordered_map<const arm64::gadgets::IndirectLink *,
		std::unordered_map<std::uint64_t, std::uint64_t>> site_misses_;
	[[nodiscard]] std::array<std::uint64_t, 8> miss_distribution() const
	{
		/* misses at sites with 1, 2, 3, 4, 5-8, 9-16, 17-64, >64 targets */
		std::array<std::uint64_t, 8> out{};
		for (const auto &[site, targets] : site_misses_) {
			std::uint64_t misses = 0;
			for (const auto &[pc, count] : targets)
				misses += count;
			const auto n = targets.size();
			const std::size_t bucket = n <= 4 ? n - 1 : n <= 8 ? 4 :
				n <= 16 ? 5 : n <= 64 ? 6 : 7;
			out[bucket] += misses;
		}
		return out;
	}
private:
#endif
	CountingResource counted_;
	BodyArena bodies_;
	/* Large enough that an ordinary build never reaches the upstream: a
	 * stream that doubles its way to 272 words consumes 510 words of
	 * scratch on the way. */
	std::array<std::byte, 32U << 10> scratch_;
	std::pmr::monotonic_buffer_resource build_scratch_;
	/* Bounded: a guest that keeps generating code would otherwise accumulate
	 * every callee it ever called. */
	static constexpr std::size_t maximum_inlined_callees = 1U << 16;
	std::unordered_map<std::uint64_t, std::uint64_t> inlined_callees_;
	/* At most uml_gadget_resident_generations + 1 entries, so finding one is
	 * cheap; the state each holds is what makes retiring it proportional. */
	std::unordered_map<unsigned long, Generation> generations_;
	/* generations_ is node-based, so this stays valid across insertion; only
	 * retiring this very address space could invalidate it, and retirement
	 * only ever takes the least recently current one. */
	Generation *current_{};
	std::vector<unsigned long> resident_;
	unsigned long current_generation_{};
#if UML_GADGET_CACHE_CENSUS
	std::uint64_t stores_{};
	mutable std::uint64_t stream_lookups_{};
#endif
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
#if UML_GADGET_CACHE_CENSUS
	if (const char *path = std::getenv("DARWIN_UML_GADGET_CACHE_CENSUS");
	    path != nullptr && *path != '\0') {
		/* Appended, because every executor of the run reports its own
		 * cache and the union across them is the measurement. */
		static std::mutex census_mutex;
		const std::lock_guard guard{census_mutex};
		if (std::FILE *out = std::fopen(path, "ae"); out != nullptr) {
			programs_->census(out);
			std::fclose(out);
		}
	}
#endif
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
	cpu.gadget_return_stack = {};
	cpu.gadget_return_top = 0;
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
	if (cache_generation_ != generation || cache_changes_ != changes) {
		programs_->invalidate();
		if (current_cpu_ != nullptr) {
			current_cpu_->gadget_return_stack = {};
			current_cpu_->gadget_return_top = 0;
		}
	}
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
	/* Translating a callee inline needs to read guest instructions somewhere
	 * other than where the build started, which only the executor can do. */
	const arm64::gadgets::InlineCalls inline_calls{
		[this](std::uint64_t callee) {
			return programs_->admit_inlined_call(callee);
		},
		[this](std::uint64_t address, std::span<std::uint32_t> into) {
			return fetch_instruction_window(*build_memory_, address,
				into);
		},
	};
	std::array<std::uint32_t, block_instruction_count> consumed;
	std::size_t consumed_count = 0;
	auto decoded = arm64::gadgets::Builder{}.build(
		pc, window,
		[this](std::uint64_t target) {
			return programs_->link(target);
		},
		programs_->program_storage(),
		[this](std::uint64_t source_pc) {
			return programs_->indirect_link(source_pc);
		},
		build_options_, &relocations,
		inline_calls_enabled && build_memory_ != nullptr ?
			&inline_calls : nullptr,
		std::span{consumed}, &consumed_count);
	if (key != nullptr)
		arm64::gadgets::SharedProgramStore::instance().publish(
			*key, decoded, relocations);
	/* With a callee inlined the words the build read are no longer a prefix
	 * of the caller's window, so the stale-program check is given what the
	 * build actually read, in run order. */
	return store(pc, std::move(decoded),
		std::span<const std::uint32_t>{consumed}.first(consumed_count));
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
	tlb_.fault_alignment = 0;
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
	tlb_.tagged_addresses = cpu.tagged_addresses_enabled();
	const auto guest_fpcr = cpu.fp_loaded ? cpu.fpcr : cpu.deferred_fpcr;
	if (single_step_ || (guest_fpcr &
	     arm64::detail::fpcr_exception_enable_mask) != 0U)
		return run_trap_enabled_instruction(cpu, memory, executed);
	if (cache_generation_ != generation) {
		/* Return records name link state of the address space being
		 * left; the ring must not survive into another. */
		cpu.gadget_return_stack = {};
		cpu.gadget_return_top = 0;
	}
	prepare_cache(generation, sequences.code_changes);
	auto selected = find(cpu.pc, sequences.code_changes, memory);
#if UML_GADGET_EXECUTION_HISTOGRAM
	++cache_diagnostics_.block_entries;
	if (!selected)
		++cache_diagnostics_.cache_misses;
#endif
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
		build_memory_ = &memory;
		selected = std::cref(translate(cpu.pc, window));
		build_memory_ = nullptr;
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
#if UML_GADGET_EXECUTION_HISTOGRAM
	{
		auto &d = cache_diagnostics_;
		if (event == ::uml::cpu::EngineEvent::system_call)
			++d.exits_syscall;
		else if (event == ::uml::cpu::EngineEvent::memory_fault)
			++d.exits_fault;
		else if (event == ::uml::cpu::EngineEvent::interrupt)
			++d.exits_interrupt;
		else if (event != ::uml::cpu::EngineEvent::continued)
			++d.exits_other;
		else if (cpu.gadget_indirect_miss != nullptr) {
			++d.exits_indirect_miss;
			if (static_cast<const arm64::gadgets::IndirectLink *>(
				    cpu.gadget_indirect_miss)->kind != 0)
				++d.exits_indirect_miss_return;
			if ((d.exits_indirect_miss & 4095) == 0)
				d.miss_by_site_targets = programs_->miss_distribution();
		}
		else if (executed >= 4096)
			++d.exits_chain_limit;
		else
			++d.exits_unlinked;
		diagnostics::execution_histogram_note_executor(&d);
	}
#endif
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
	/* Guest trap enables and single steps are intentionally a cold
	 * execution mode.  Limiting every translated run to one instruction
	 * guarantees that an integer or memory instruction cannot carry
	 * execution past a later FP/SIMD data-processing instruction in the
	 * same cached block, or past the instruction being stepped.
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
		 * live architectural state.
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

bool GadgetExecutor::fault_alignment() const noexcept
{
	return tlb_.fault_alignment != 0;
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
