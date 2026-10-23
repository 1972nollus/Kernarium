// SPDX-License-Identifier: GPL-2.0-or-later
/* The paths the JIT executor's A64 and AArch32 entries run only when a block
 * is missing, when another vCPU is publishing, or when a step must be small:
 *
 * - a second executor reuses the first one's A32 and T32 translations, and
 *   still finds code rewritten under them;
 * - while another vCPU holds a reservation, a block that stores runs as its
 *   tracked variant, built from the code now in memory, and falls back to one
 *   instruction when that code no longer matches the block;
 * - a single step, and an entry in the middle of an IT block, run one
 *   instruction as its own translation, tracked or not;
 * - a fetch that cannot supply the first instruction reports the fault at the
 *   first unit it could not read, whichever entry or fetch ran. */
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_publication.hpp"
#include "cpu/arm64/jit/jit_shared_translations.hpp"
#include "memory/static_guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

using namespace uml::emulator::tests;

namespace {

namespace arm64 = uml::cpu::arm64;
namespace key = uml::cpu::arm64::aarch32;
using uml::cpu::EngineEvent;
using uml::cpu::arm64::JitExecutor;
using uml::memory::Access;
using uml::memory::StaticGuestMemory;

constexpr std::size_t page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::uint32_t code_base = 0x00f0'0000U;
constexpr std::uint32_t data_offset = 2U * page;
constexpr std::uint32_t stored_value = 0x1234'5678U;

constexpr std::uint32_t a32_add_r0 = 0xe280'0001U;  // add r0, r0, #1
constexpr std::uint32_t a32_str = 0xe582'1000U;     // str r1, [r2]
constexpr std::uint32_t a32_str_4 = 0xe582'1004U;   // str r1, [r2, #4]
constexpr std::uint32_t a32_svc = 0xef00'0000U;     // svc #0
constexpr std::uint16_t t32_adds_r0 = 0x3001;       // adds r0, #1
constexpr std::uint16_t t32_str = 0x6011;           // str r1, [r2]
constexpr std::uint16_t t32_svc = 0xdf00;           // svc #0
constexpr std::uint16_t t32_str_w[] = {0xf8c2, 0x1000};   // str.w r1, [r2]
constexpr std::uint16_t t32_str_w_4[] = {0xf8c2, 0x1004}; // str.w r1, [r2, #4]
constexpr std::uint32_t a64_add_x0 = 0x9100'0400U;  // add x0, x0, #1
constexpr std::uint32_t a64_str = 0xf900'0041U;     // str x1, [x2]
constexpr std::uint32_t a64_svc = 0xd400'0001U;     // svc #0

enum class Isa { a64, a32, t32 };

/* A code region of `pages` pages at `base` (the page after the code is data
 * when `pages` is three), writable and executable, in a generation of its
 * own. */
class Region final {
public:
	Region(unsigned long generation, std::uint32_t base, std::size_t pages = 3)
		: generation_{generation}, base_{base}, backing_{pages},
		  mapping_{generation, base, backing_, pages}
	{
	}

	void put32(std::size_t offset, std::uint32_t word)
	{
		backing_.put32(offset, word);
	}
	void put16(std::size_t offset, std::uint16_t half)
	{
		backing_.put(offset, std::as_bytes(std::span{&half, 1}));
	}
	[[nodiscard]] std::uint32_t get32(std::size_t offset) const
	{
		std::uint32_t word;
		backing_.get(offset, std::as_writable_bytes(std::span{&word, 1}));
		return word;
	}
	/* Code written by a guest store, which publishes a change. */
	void changed(std::size_t offset, std::size_t length) const
	{
		require(darwin_uml_static_vm_invalidate_code_generation(
			generation_, base_ + offset, length) == 0);
	}

	[[nodiscard]] unsigned long generation() const noexcept
	{
		return generation_;
	}
	[[nodiscard]] std::uint32_t base() const noexcept { return base_; }

private:
	unsigned long generation_;
	std::uint32_t base_;
	JitBacking backing_;
	JitMapping mapping_;
};

/* The task about to run `isa` code at `address`: r1 holds the value its store
 * writes and r2 (x2) the data word. */
[[nodiscard]] arm64::CpuState task(Isa isa, std::uint32_t address,
	std::uint32_t base)
{
	arm64::CpuState cpu{};
	cpu.registers[1] = stored_value;
	cpu.registers[2] = base + data_offset;
	if (isa == Isa::a64) {
		cpu.pc = address;
		return cpu;
	}
	cpu.a32 = 1;
	cpu.pc = isa == Isa::t32 ? key::t32_key(address) : key::a32_key(address);
	return cpu;
}

struct Outcome final {
	EngineEvent event{};
	std::size_t executed{};
};

/* One entry into `executor`, through the A64 or the AArch32 block entry the
 * task's ISA selects. */
[[nodiscard]] Outcome enter(JitExecutor &executor, StaticGuestMemory &memory,
	unsigned long generation, Isa isa, arm64::CpuState &cpu)
{
	require(executor.begin_execution(generation, cpu.pc));
	Outcome outcome;
	outcome.event = isa == Isa::a64 ?
		executor.run_block(cpu, memory, generation, outcome.executed) :
		executor.run_a32_block(cpu, memory, generation, outcome.executed);
	executor.end_execution();
	return outcome;
}

/* Entries until the program's SVC; the sum of the retired counts. */
[[nodiscard]] std::size_t run_to_svc(JitExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, Isa isa,
	arm64::CpuState &cpu)
{
	std::size_t retired = 0;
	for (unsigned entry = 0; entry < 16; ++entry) {
		const auto outcome = enter(executor, memory, generation, isa, cpu);
		retired += outcome.executed;
		if (outcome.event == EngineEvent::system_call)
			return retired;
		require(outcome.event == EngineEvent::continued);
	}
	require(false);
	return retired;
}

/* The ISA's program: add; store; add; svc (four instructions). The T32 form
 * stores with the 32-bit encoding so that it holds five halfwords. */
void write_tracking_program(Region &region, Isa isa, std::size_t offset = 0)
{
	switch (isa) {
	case Isa::a64:
		region.put32(offset, a64_add_x0);
		region.put32(offset + 4, a64_str);
		region.put32(offset + 8, a64_add_x0);
		region.put32(offset + 12, a64_svc);
		break;
	case Isa::a32:
		region.put32(offset, a32_add_r0);
		region.put32(offset + 4, a32_str);
		region.put32(offset + 8, a32_add_r0);
		region.put32(offset + 12, a32_svc);
		break;
	case Isa::t32:
		region.put16(offset, t32_adds_r0);
		region.put16(offset + 2, t32_str_w[0]);
		region.put16(offset + 4, t32_str_w[1]);
		region.put16(offset + 6, t32_adds_r0);
		region.put16(offset + 8, t32_svc);
		break;
	}
	region.changed(offset, 16);
}

[[nodiscard]] const char *name(Isa isa) noexcept
{
	return isa == Isa::a64 ? "A64" : isa == Isa::a32 ? "A32" : "T32";
}

/* The value the program's store left in the data word, and its reset. */
[[nodiscard]] std::uint32_t data_word(const Region &region)
{
	return region.get32(data_offset);
}

/* A reservation another vCPU holds makes every direct store publish its
 * ranges: the executor runs the tracked variant of a block that stores. */
class ForeignReservation final {
public:
	ForeignReservation() { require(arm64::begin_jit_reservation(cpu_)); }
	~ForeignReservation() { arm64::end_jit_reservation(cpu_); }
	ForeignReservation(const ForeignReservation &) = delete;
	ForeignReservation &operator=(const ForeignReservation &) = delete;

private:
	arm64::CpuState cpu_{};
};

/* A block that stores runs untracked while nobody reserves, and as its
 * tracked variant while another vCPU does; both leave the same state. */
void tracked_variant_runs_the_block(Isa isa, unsigned long generation)
{
	Region region{generation, code_base};
	write_tracking_program(region, isa);
	JitExecutor executor;
	StaticGuestMemory memory;
	const auto address = code_base;
	{
		auto cpu = task(isa, address, code_base);
		require(run_to_svc(executor, memory, generation, isa, cpu) == 4);
		require(cpu.registers[0] == 2);
		require(data_word(region) == stored_value);
	}
	region.put32(data_offset, 0);
	{
		ForeignReservation foreign;
		for (unsigned round = 0; round < 2; ++round) {
			auto cpu = task(isa, address, code_base);
			require(run_to_svc(executor, memory, generation, isa, cpu) == 4);
			require(cpu.registers[0] == 2);
			require(data_word(region) == stored_value);
			region.put32(data_offset, 0);
		}
	}
	std::printf("tracked variant runs the %s block\n", name(isa));
}

/* The tracked variant is built again from the code in memory. Code rewritten
 * without a published change leaves the cached block valid for an untracked
 * run, but a tracked run compiles something else and so makes progress one
 * instruction at a time instead. */
void tracked_variant_that_no_longer_matches_runs_one_instruction(Isa isa,
	unsigned long generation, bool same_instruction_count = false)
{
	Region region{generation, code_base};
	write_tracking_program(region, isa);
	JitExecutor executor;
	StaticGuestMemory memory;
	const auto address = code_base;
	{
		auto cpu = task(isa, address, code_base);
		require(run_to_svc(executor, memory, generation, isa, cpu) == 4);
	}
	region.put32(data_offset, 0);
	/* Rewrite behind the cache's back: the third instruction becomes the SVC,
	 * or, for T32, the 32-bit store becomes a 16-bit one and the SVC moves
	 * up, which keeps the instruction count and changes the halfword count. */
	if (isa == Isa::a64) {
		region.put32(8, a64_svc);
	} else if (isa == Isa::a32) {
		region.put32(8, a32_svc);
	} else if (same_instruction_count) {
		region.put16(2, t32_str);
		region.put16(4, t32_adds_r0);
		region.put16(6, t32_svc);
	} else {
		region.put16(6, t32_svc);
	}
	{
		/* Nobody reserves: the cached block runs as it was. */
		auto cpu = task(isa, address, code_base);
		require(run_to_svc(executor, memory, generation, isa, cpu) == 4);
		require(cpu.registers[0] == 2);
		require(data_word(region) == stored_value);
		region.put32(data_offset, 0);
	}
	{
		ForeignReservation foreign;
		auto cpu = task(isa, address, code_base);
		const auto outcome = enter(executor, memory, generation, isa, cpu);
		/* The block's first instruction, as its own translation. */
		require(outcome.event == EngineEvent::continued);
		require(outcome.executed == 1);
		require(cpu.registers[0] == 1);
		require(data_word(region) == 0);
		/* And the rest runs on from there. */
		const auto rest = run_to_svc(executor, memory, generation, isa, cpu);
		require(rest >= 2);
		require(data_word(region) == stored_value);
	}
	std::printf("tracked variant of a changed %s block%s runs one instruction\n",
		name(isa), same_instruction_count ? " (same count)" : "");
}

/* A step that stores runs as a one-instruction translation, and the tracked
 * form of it while another vCPU reserves. */
void single_step_stores(Isa isa, unsigned long generation)
{
	Region region{generation, code_base};
	write_tracking_program(region, isa);
	JitExecutor executor;
	executor.set_single_step(true);
	StaticGuestMemory memory;
	for (const bool tracked : {false, true}) {
		region.put32(data_offset, 0);
		auto cpu = task(isa, code_base, code_base);
		std::size_t entries = 0;
		std::size_t retired = 0;
		const auto step = [&] {
			const auto outcome = enter(executor, memory, generation, isa, cpu);
			require(outcome.executed == 1);
			retired += outcome.executed;
			++entries;
			return outcome.event;
		};
		const auto run = [&] {
			EngineEvent event = EngineEvent::continued;
			while (event == EngineEvent::continued && entries < 8)
				event = step();
			require(event == EngineEvent::system_call);
			require(retired == 4 && entries == 4);
			require(cpu.registers[0] == 2);
			require(data_word(region) == stored_value);
		};
		if (tracked) {
			ForeignReservation foreign;
			run();
		} else {
			run();
		}
	}
	std::printf("single-stepped %s stores\n", name(isa));
}

/* An entry in the middle of an IT block runs one instruction as its own
 * translation with the ITSTATE it holds, and leaves the ITSTATE that follows.
 * ITSTATE 0xe8 is "IT AL": the store is the block's only instruction. */
void it_block_entry_stores(bool tracked, unsigned long generation)
{
	Region region{generation, code_base};
	region.put16(0, t32_str);
	region.put16(2, t32_svc);
	region.changed(0, 4);
	JitExecutor executor;
	StaticGuestMemory memory;
	auto cpu = task(Isa::t32, code_base, code_base);
	cpu.a32_itstate = 0xe8U;
	const auto run = [&] {
		const auto outcome =
			enter(executor, memory, generation, Isa::t32, cpu);
		require(outcome.event == EngineEvent::continued);
		require(outcome.executed == 1);
		require(cpu.a32_itstate == 0);
		require(data_word(region) == stored_value);
	};
	if (tracked) {
		ForeignReservation foreign;
		run();
	} else {
		run();
	}
	std::printf("IT-block entry stores%s\n", tracked ? " (tracked)" : "");
}

/* A second executor installs what the first published, for A32 and for T32
 * (whose instructions are one or two halfwords, and whose identity is a
 * count of halfwords), and a rewrite of the code under it is found: of the
 * store, or of the last unit of the identity, which a count that fell short
 * would not compare. */
void a_peer_reuses_aarch32_translations(Isa isa, bool even_units,
	bool rewrite_last, unsigned long generation, std::uint32_t base)
{
	Region region{generation, base};
	/* add; store; svc. The T32 form is four halfwords (the 32-bit store
	 * between two 16-bit instructions), or, without the add, three: an odd
	 * identity that does not fill its last 32-bit element. */
	if (isa == Isa::a32) {
		region.put32(0, a32_add_r0);
		region.put32(4, a32_str);
		region.put32(8, a32_svc);
	} else if (even_units) {
		region.put16(0, t32_adds_r0);
		region.put16(2, t32_str_w[0]);
		region.put16(4, t32_str_w[1]);
		region.put16(6, t32_svc);
	} else {
		region.put16(0, t32_str_w[0]);
		region.put16(2, t32_str_w[1]);
		region.put16(4, t32_svc);
	}
	region.changed(0, 16);
	auto &shared = arm64::SharedTranslationCache::instance();
	StaticGuestMemory memory;
	const auto before = shared.statistics();
	/* Sharing needs two live executors; the second runs only after the
	 * first is gone. */
	JitExecutor second;
	const std::size_t instructions = isa == Isa::a32 || even_units ? 3 : 2;
	{
		JitExecutor first;
		auto cpu = task(isa, base, base);
		require(run_to_svc(first, memory, generation, isa, cpu) ==
			instructions);
		require(data_word(region) == stored_value);
	}
	const auto published = shared.statistics();
	require(published.insertions > before.insertions);
	region.put32(data_offset, 0);
	{
		auto cpu = task(isa, base, base);
		require(run_to_svc(second, memory, generation, isa, cpu) ==
			instructions);
		require(data_word(region) == stored_value);
		if (isa == Isa::a32 || even_units)
			require(cpu.registers[0] == 1);
	}
	const auto reused = shared.statistics();
	require(reused.reuses > published.reuses);

	region.put32(data_offset, 0);
	if (rewrite_last) {
		/* The SVC becomes an undefined instruction. */
		const std::size_t last = isa == Isa::a32 ? 8 : even_units ? 6 : 4;
		if (isa == Isa::a32) {
			region.put32(last, 0xe7f0'00f0U); // udf #0
			region.changed(last, 4);
		} else {
			region.put16(last, 0xde00); // udf #0
			region.changed(last, 2);
		}
		auto cpu = task(isa, base, base);
		const auto outcome = enter(second, memory, generation, isa, cpu);
		require(outcome.event == EngineEvent::illegal_instruction);
	} else {
		/* The store's displacement changes, and publishes the change: the
		 * second executor, which holds the reused block, finds it. */
		if (isa == Isa::a32) {
			region.put32(4, a32_str_4);
			region.changed(4, 4);
		} else {
			const std::size_t at = even_units ? 2 : 0;
			region.put16(at, t32_str_w_4[0]);
			region.put16(at + 2, t32_str_w_4[1]);
			region.changed(at, 4);
		}
		auto cpu = task(isa, base, base);
		require(run_to_svc(second, memory, generation, isa, cpu) ==
			instructions);
		require(region.get32(data_offset) == 0);
		require(region.get32(data_offset + 4) == stored_value);
	}
	std::printf("peer reuses %s%s translations, rewrites the %s\n", name(isa),
		isa == Isa::t32 ? (even_units ? " (even)" : " (odd)") : "",
		rewrite_last ? "last unit" : "store");
}

/* The first instruction cannot be fetched: the fault is at the first unit
 * the fetch could not read, an execute access, and nothing retires. */
void a_fetch_fault_is_reported(Isa isa, bool single_step,
	unsigned long generation)
{
	/* One page of code: the instruction under test is the last word (A32) or
	 * 32-bit pair of halfwords (T32), starting a halfword before the end. */
	Region region{generation, code_base, 1};
	const std::size_t offset = page - (isa == Isa::t32 ? 2U : 0U);
	if (isa == Isa::t32)
		region.put16(offset, t32_str_w[0]);
	region.changed(0, page);
	JitExecutor executor;
	executor.set_single_step(single_step);
	StaticGuestMemory memory;
	for (const bool unmapped_entry : {true, false}) {
		const std::uint32_t address = unmapped_entry ?
			code_base + 8U * static_cast<std::uint32_t>(page) :
			code_base + static_cast<std::uint32_t>(offset);
		if (!unmapped_entry && isa == Isa::a32)
			continue;
		auto cpu = task(isa, address, code_base);
		const auto before = cpu.pc;
		const auto outcome = enter(executor, memory, generation, isa, cpu);
		require(outcome.event == EngineEvent::memory_fault);
		require(outcome.executed == 0);
		require(cpu.pc == before);
		require(executor.fault_access() == Access::execute);
		/* The unmapped page, or the page after the straddling halfword. */
		require(executor.fault_address() ==
			(unmapped_entry ? address : code_base + page));
	}
	std::printf("%s fetch fault%s\n", name(isa),
		single_step ? " (single step)" : "");
}

/* A block ends where the mapped code does: the SVC is the last word of the
 * only page, and the fetch past it is a fault the block never reports. */
void a_block_ends_at_the_end_of_the_mapping(Isa isa, unsigned long generation)
{
	Region region{generation, code_base, 1};
	std::size_t offset = page - (isa == Isa::t32 ? 4U : 8U);
	if (isa == Isa::t32) {
		region.put16(offset, t32_adds_r0);
		region.put16(offset + 2, t32_svc);
	} else {
		region.put32(offset, isa == Isa::a32 ? a32_add_r0 : a64_add_x0);
		region.put32(offset + 4, isa == Isa::a32 ? a32_svc : a64_svc);
	}
	region.changed(0, page);
	JitExecutor executor;
	StaticGuestMemory memory;
	auto cpu = task(isa, code_base + static_cast<std::uint32_t>(offset),
		code_base);
	require(run_to_svc(executor, memory, generation, isa, cpu) == 2);
	require(cpu.registers[0] == 1);
	std::printf("%s block ends at the end of the mapping\n", name(isa));
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	constexpr std::size_t gib = std::size_t{1024} * 1024U * 1024U;
	require(arm64::set_jit_cache_budget(1U * gib, 3));
	unsigned long generation = 0x30a3'2000UL;
	for (const auto isa : {Isa::a64, Isa::a32, Isa::t32}) {
		tracked_variant_runs_the_block(isa, generation++);
		tracked_variant_that_no_longer_matches_runs_one_instruction(
			isa, generation++);
		single_step_stores(isa, generation++);
		a_block_ends_at_the_end_of_the_mapping(isa, generation++);
	}
	tracked_variant_that_no_longer_matches_runs_one_instruction(
		Isa::t32, generation++, true);
	it_block_entry_stores(false, generation++);
	it_block_entry_stores(true, generation++);
	std::uint32_t base = code_base;
	for (const bool rewrite_last : {false, true}) {
		a_peer_reuses_aarch32_translations(Isa::a32, false, rewrite_last,
			generation++, base += 0x10000U);
		a_peer_reuses_aarch32_translations(Isa::t32, true, rewrite_last,
			generation++, base += 0x10000U);
		a_peer_reuses_aarch32_translations(Isa::t32, false, rewrite_last,
			generation++, base += 0x10000U);
	}
	for (const auto isa : {Isa::a32, Isa::t32})
		for (const bool single_step : {false, true})
			a_fetch_fault_is_reported(isa, single_step, generation++);
	return 0;
}
