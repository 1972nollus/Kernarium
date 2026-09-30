// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"

#include "cpu/arm64/gadgets/executor.hpp"
#include "static_mmu_test_support.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#ifndef UML_GADGET_CACHE_SLOTS
#error "gadget cache validation test needs the configured cache geometry"
#endif

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::emulator::tests::require;
using uml::memory::StaticGuestMemory;

namespace {

constexpr std::uint64_t code_base = 0xd0'0000;
constexpr std::uint32_t system_call = 0xd400'0001U;

struct Outcome final {
	::uml::cpu::EngineEvent event;
	std::size_t executed;
};

[[nodiscard]] Outcome run_once(gadgets::GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, arm64::CpuState &cpu)
{
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto event = executor.run_block(cpu, memory, generation, executed);
	executor.end_execution();
	return {event, executed};
}

[[nodiscard]] constexpr std::uint32_t move_immediate(
	unsigned reg, unsigned value) noexcept
{
	return 0xd280'0000U | ((value & 0xffffU) << 5U) | (reg & 31U);
}

[[nodiscard]] std::uint32_t branch(
	std::uint64_t source, std::uint64_t target)
{
	const auto displacement = static_cast<std::int64_t>(target) -
		static_cast<std::int64_t>(source);
	require((displacement & 3) == 0);
	return 0x1400'0000U |
		(static_cast<std::uint32_t>(displacement >> 2) & 0x03ff'ffffU);
}

void write_mapped32(unsigned long generation, std::uint64_t address,
	std::uint32_t value)
{
	void *destination = nullptr;
	require(darwin_uml_static_vm_translate(
		generation, address, DARWIN_UML_STATIC_WRITE, &destination) ==
		DARWIN_UML_STATIC_TRANSLATED);
	std::memcpy(destination, &value, sizeof(value));
}

void unchanged_words_avoid_retranslation()
{
	constexpr unsigned long generation = 920;
	JitBacking backing{2};
	backing.put32(0, move_immediate(0, 7));
	backing.put32(4, system_call);
	JitMapping mapping{generation, code_base, backing, 2};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 7);

	darwin_uml_static_vm_invalidate_code(
		code_base + DARWIN_UML_GUEST_PAGE_SIZE);
	cpu = {};
	cpu.pc = code_base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 7);
	const auto diagnostics = executor.cache_diagnostics();
	require(diagnostics.translations == 1);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_hits == 1);
}

void changed_words_rebuild_and_republish_links()
{
	constexpr unsigned long generation = 921;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, branch(code_base, target));
	backing.put32(target - code_base, move_immediate(0, 1));
	backing.put32(target - code_base + 4, system_call);
	JitMapping mapping{generation, code_base, backing, 1};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;

	arm64::CpuState cpu{};
	cpu.pc = target;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	cpu = {};
	cpu.pc = code_base;
	const auto linked = run_once(executor, memory, generation, cpu);
	require(linked.event == ::uml::cpu::EngineEvent::system_call);
	require(linked.executed == 3);
	require(cpu.registers[0] == 1);

	write_mapped32(generation, target, move_immediate(0, 2));
	darwin_uml_static_vm_invalidate_code(target);
	cpu = {};
	cpu.pc = code_base;
	const auto reset_link = run_once(executor, memory, generation, cpu);
	require(reset_link.event == ::uml::cpu::EngineEvent::continued);
	require(reset_link.executed == 1);
	require(cpu.pc == target);

	const auto rebuilt = run_once(executor, memory, generation, cpu);
	require(rebuilt.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 2);
	cpu = {};
	cpu.pc = code_base;
	const auto relinked = run_once(executor, memory, generation, cpu);
	require(relinked.event == ::uml::cpu::EngineEvent::system_call);
	require(relinked.executed == 3);
	require(cpu.registers[0] == 2);

	const auto diagnostics = executor.cache_diagnostics();
	require(diagnostics.translations == 3);
	require(diagnostics.validation_attempts == 2);
	require(diagnostics.validation_hits == 1);
	require(diagnostics.validation_mismatches == 1);
}

/* Linux writes a traced process's text through its own view of the frame --
 * ptrace inserting a breakpoint -- so no translated store publishes it. The
 * kernel then names the process's generation and the written range through
 * darwin_uml_vm_invalidate_code(); the page
 * stamp must move and the next run must execute the new instruction. */
void a_kernel_text_write_republishes_through_the_bridge()
{
	constexpr unsigned long generation = 922;
	JitBacking backing{1};
	backing.put32(0, move_immediate(0, 1));
	backing.put32(4, system_call);
	JitMapping mapping{generation, code_base, backing, 1};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 1);

	struct darwin_uml_static_code_page_stamp before{};
	void *handle = nullptr;
	require(darwin_uml_static_vm_code_page(generation, code_base, &handle,
		&before) == DARWIN_UML_STATIC_TRANSLATED);
	backing.put32(0, move_immediate(0, 2));
	require(darwin_uml_static_vm_invalidate_code_generation(
		generation, code_base, 4) == 0);
	struct darwin_uml_static_code_page_stamp after{};
	require(darwin_uml_static_vm_code_page(generation, code_base, &handle,
		&after) == DARWIN_UML_STATIC_TRANSLATED);
	require(after.identity == before.identity &&
		after.version != before.version);

	cpu = {};
	cpu.pc = code_base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 2);

	/* process_vm_writev() reports its data transfers the same way; a page
	 * mapped without execute permission holds no translation to retire. */
	constexpr auto data_base = code_base + 0x10'0000;
	JitBacking data{1};
	require(darwin_uml_static_vm_map(reinterpret_cast<void *>(data_base),
		data.descriptor(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	const auto code_changes = darwin_uml_static_vm_code_changes(generation);
	require(darwin_uml_static_vm_invalidate_code_generation(
		generation, data_base, DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	require(darwin_uml_static_vm_code_changes(generation) == code_changes);
	require(darwin_uml_static_vm_invalidate_code_generation(
		generation, code_base, 4) == 0);
	require(darwin_uml_static_vm_code_changes(generation) != code_changes);
}

/* A code change resets every activation, not only the ones it touched.
 *
 * The guest code-change sequence carries no range, so a change anywhere has to
 * be treated as a change everywhere. This invalidates a page that holds none of
 * the executed code and leaves every word intact, then requires the successor
 * link to have been dropped all the same: the linked run must fall back to
 * exiting at its successor instead of continuing into it.
 *
 * The reset walks only the records that hold an activation rather than every
 * record ever created, so this also pins that the narrower walk still reaches a
 * record the change did not name.
 */
void an_unrelated_code_change_resets_an_untouched_link()
{
	constexpr unsigned long generation = 922;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, branch(code_base, target));
	backing.put32(target - code_base, move_immediate(0, 5));
	backing.put32(target - code_base + 4, system_call);
	JitMapping mapping{generation, code_base, backing, 1};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;

	arm64::CpuState cpu{};
	cpu.pc = target;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	cpu = {};
	cpu.pc = code_base;
	const auto linked = run_once(executor, memory, generation, cpu);
	require(linked.event == ::uml::cpu::EngineEvent::system_call);
	require(linked.executed == 3);
	require(cpu.registers[0] == 5);

	darwin_uml_static_vm_invalidate_code(
		code_base + DARWIN_UML_GUEST_PAGE_SIZE);
	cpu = {};
	cpu.pc = code_base;
	const auto reset_link = run_once(executor, memory, generation, cpu);
	require(reset_link.event == ::uml::cpu::EngineEvent::continued);
	require(reset_link.executed == 1);
	require(cpu.pc == target);

	/* The words never changed, so both blocks revalidate and relink rather
	 * than retranslating.
	 */
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	cpu = {};
	cpu.pc = code_base;
	const auto relinked = run_once(executor, memory, generation, cpu);
	require(relinked.event == ::uml::cpu::EngineEvent::system_call);
	require(relinked.executed == 3);
	require(cpu.registers[0] == 5);
	const auto diagnostics = executor.cache_diagnostics();
	require(diagnostics.translations == 2);
	require(diagnostics.validation_hits == diagnostics.validation_attempts);
	require(diagnostics.validation_mismatches == 0);
}

/* Retiring the least recently current address space drops its translations and
 * keeps everybody else's.
 *
 * Retirement walks the slot list the retired address space kept while it was
 * storing rather than the whole cache, so a slot it names but no longer owns
 * must be left alone, and a slot it owns must not be missed. Running one more
 * address space than the residency cap forces exactly one retirement, and the
 * surviving spaces then have to answer from the cache while the retired one has
 * to translate again.
 */
void retiring_an_address_space_keeps_the_others()
{
	constexpr unsigned long first = 940;
	constexpr auto resident = UML_GADGET_RESIDENT_GENERATIONS;
	JitBacking backing{1};
	backing.put32(0, move_immediate(0, 7));
	backing.put32(4, system_call);

	auto executor = std::make_unique<gadgets::GadgetExecutor>();
	auto memory = std::make_unique<StaticGuestMemory>();
	std::vector<std::unique_ptr<JitMapping>> mappings;
	const auto visit = [&](unsigned long generation) {
		arm64::CpuState cpu{};
		cpu.pc = code_base;
		const auto outcome =
			run_once(*executor, *memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == 7);
	};

	/* Fill residency exactly, then run them all again: everything is a hit,
	 * so nothing has been retired yet. */
	for (unsigned long index = 0; index < resident; ++index) {
		mappings.push_back(std::make_unique<JitMapping>(
			first + index, code_base, backing, 1));
		visit(first + index);
	}
	const auto after_fill = executor->cache_diagnostics().translations;
	require(after_fill == resident);
	for (unsigned long index = 0; index < resident; ++index)
		visit(first + index);
	require(executor->cache_diagnostics().translations == after_fill);

	/* One more address space retires the least recently current, which is
	 * the one that has been idle longest -- `first`. */
	mappings.push_back(std::make_unique<JitMapping>(
		first + resident, code_base, backing, 1));
	visit(first + resident);
	require(executor->cache_diagnostics().translations == after_fill + 1);

	/* Every space except the retired one still answers from the cache. */
	for (unsigned long index = 1; index < resident; ++index)
		visit(first + index);
	require(executor->cache_diagnostics().translations == after_fill + 1);

	/* The retired one has to be translated again, and retires another. */
	visit(first);
	require(executor->cache_diagnostics().translations == after_fill + 2);
}

/* Mirrors ProgramCache::locate(): the set a translation lands in folds the
 * address space into the index, so the only way to aim several address spaces
 * at one set of ways from outside is to choose generations that collide there.
 */
constexpr std::size_t cache_ways = 4;
constexpr std::size_t cache_sets = UML_GADGET_CACHE_SLOTS / cache_ways;

[[nodiscard]] constexpr std::size_t cache_set(
	unsigned long generation, std::uint64_t pc) noexcept
{
	const auto block = (pc >> 2) ^
		(static_cast<std::uint64_t>(generation) *
			0x9e37'79b9'7f4a'7c15ULL);
	const auto mixed = block ^ (block >> 12) ^ (block >> 24);
	return static_cast<std::size_t>(mixed) & (cache_sets - 1);
}

/* A slot handed to another address space must not keep the previous owner's
 * link record.
 *
 * A slot memoises the record its translation publishes into, and all
 * activate() can ask of that memo is the record's guest PC and the cache
 * epoch -- neither of which changes when the slot is taken by a *different*
 * address space at the same guest PC. Reusing it there publishes the new
 * occupant's translation into the previous owner's record, so the previous
 * owner's own code branches straight into another address space's
 * translation, against G160; and once that owner retires, the record its map
 * held is destroyed while the slot still names it, which is a live executor
 * dereferencing freed storage on its next activation.
 *
 * Five address spaces are chosen so that one guest PC lands in the same set of
 * four ways in all of them, and so that the predecessor block lands elsewhere
 * and survives the set filling up. The first four fill the set; the fifth
 * stores at the same guest PC and takes the first one's way, the set's
 * round-robin replacement having come back around to it. The first address
 * space then has to find its successor link reset rather than pointing at the
 * fifth's translation.
 */
void a_replaced_slot_drops_the_previous_owner_link()
{
	constexpr auto target = code_base + 0x100;
	constexpr unsigned owner_value = 7;
	constexpr unsigned other_value = 9;
	constexpr std::size_t spaces_wanted = cache_ways + 1;

	/* All of them have to be resident at once: retiring one would release
	 * its slots and destroy its records, which is the sequence this is here
	 * to keep unreachable. */
	if (static_cast<std::size_t>(UML_GADGET_RESIDENT_GENERATIONS) <
	    spaces_wanted)
		return;

	std::vector<unsigned long> spaces;
	for (unsigned long generation = 1000;
	     spaces.size() < spaces_wanted && generation < 8'000'000UL;
	     ++generation) {
		const auto set = cache_set(generation, target);
		if (set == cache_set(generation, code_base))
			continue;
		if (!spaces.empty() && set != cache_set(spaces.front(), target))
			continue;
		spaces.push_back(generation);
	}
	require(spaces.size() == spaces_wanted);

	JitBacking owner_backing{1};
	owner_backing.put32(0, branch(code_base, target));
	owner_backing.put32(target - code_base, move_immediate(0, owner_value));
	owner_backing.put32(target - code_base + 4, system_call);
	JitBacking other_backing{1};
	other_backing.put32(target - code_base, move_immediate(0, other_value));
	other_backing.put32(target - code_base + 4, system_call);

	auto executor = std::make_unique<gadgets::GadgetExecutor>();
	auto memory = std::make_unique<StaticGuestMemory>();
	std::vector<std::unique_ptr<JitMapping>> mappings;
	mappings.push_back(std::make_unique<JitMapping>(
		spaces.front(), code_base, owner_backing, 1));
	for (std::size_t index = 1; index < spaces.size(); ++index)
		mappings.push_back(std::make_unique<JitMapping>(
			spaces[index], code_base, other_backing, 1));

	const auto owner = spaces.front();
	const auto run_successor = [&](unsigned long generation, unsigned value) {
		arm64::CpuState cpu{};
		cpu.pc = target;
		const auto outcome =
			run_once(*executor, *memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == value);
	};

	/* The owner translates its successor and the predecessor that links to
	 * it, and the live link carries the predecessor straight through. */
	run_successor(owner, owner_value);
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	const auto linked = run_once(*executor, *memory, owner, cpu);
	require(linked.event == ::uml::cpu::EngineEvent::system_call);
	require(linked.executed == 3);
	require(cpu.registers[0] == owner_value);

	/* Fill the remaining three ways, then take the owner's. */
	for (std::size_t index = 1; index < spaces.size(); ++index)
		run_successor(spaces[index], other_value);
	const auto before = executor->cache_diagnostics().translations;
	require(before == spaces.size() + 1);

	/* The owner's successor went with its way, so the predecessor has to
	 * exit at the successor address. Continuing here would mean the link
	 * record still names a translation -- and the only translation left at
	 * that address belongs to the address space that took the way. */
	cpu = {};
	cpu.pc = code_base;
	const auto reset_link = run_once(*executor, *memory, owner, cpu);
	require(reset_link.event == ::uml::cpu::EngineEvent::continued);
	require(reset_link.executed == 1);
	require(cpu.pc == target);

	/* Rebuilding the successor is what proves the owner's way really had
	 * been taken, and republishes the owner's own record. */
	const auto rebuilt = run_once(*executor, *memory, owner, cpu);
	require(rebuilt.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == owner_value);
	require(executor->cache_diagnostics().translations == before + 1);

	cpu = {};
	cpu.pc = code_base;
	const auto relinked = run_once(*executor, *memory, owner, cpu);
	require(relinked.event == ::uml::cpu::EngineEvent::system_call);
	require(relinked.executed == 3);
	require(cpu.registers[0] == owner_value);

	/* No guest word and no code-change sequence moved anywhere here, so
	 * nothing revalidated. That is worth pinning: a code change would reset
	 * the owner's links on its own and make the check above pass whatever
	 * the replacement did with the record.
	 */
	require(executor->cache_diagnostics().validation_attempts == 0);
}

} // namespace

int main()
{
	unchanged_words_avoid_retranslation();
	changed_words_rebuild_and_republish_links();
	a_kernel_text_write_republishes_through_the_bridge();
	an_unrelated_code_change_resets_an_untouched_link();
	retiring_an_address_space_keeps_the_others();
	a_replaced_slot_drops_the_previous_owner_link();
	return 0;
}
