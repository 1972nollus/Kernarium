// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"

#include "cpu/arm64/gadgets/executor.hpp"
#include "cpu/arm64/gadgets/shared_programs.hpp"

#include <cstdint>
#include <cstring>
#include <memory>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::emulator::tests::require;
using uml::memory::StaticGuestMemory;

namespace {

constexpr std::uint64_t code_base = 0xd0'0000;
constexpr std::uint32_t system_call = 0xd400'0001U;

/* An executor carries a megabyte of TLB and guest memory another three
 * quarters, and these cases want two or three of each at once. */
using Executor = std::unique_ptr<gadgets::GadgetExecutor>;
using Memory = std::unique_ptr<StaticGuestMemory>;

[[nodiscard]] Executor make_executor(gadgets::BuildOptions options = {})
{
	return std::make_unique<gadgets::GadgetExecutor>(options);
}

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

/* A block that branches to a successor, so the translation carries the one
 * thing that cannot simply be copied between executors: a cell holding a
 * pointer to the producer's own link record. */
void write_linked_pair(JitBacking &backing, std::uint64_t target,
	unsigned value)
{
	backing.put32(0, branch(code_base, target));
	backing.put32(target - code_base, move_immediate(0, value));
	backing.put32(target - code_base + 4, system_call);
}

/* Nothing is published until a second executor exists to adopt it, so every
 * case here keeps one alive that does no work of its own. */
struct SharingEnabled final {
	SharingEnabled() { gadgets::SharedProgramStore::instance().clear(); }

	Executor witness{make_executor()};
};

/* An adopted translation must not name anything the producer owned.
 *
 * The producer is destroyed before the consumer runs a single instruction, so
 * every BlockLink the producer resolved while building is freed memory by the
 * time the adopted stream executes. A relocation this misses is therefore not a
 * silent wrong answer but a branch through a dangling pointer, which the
 * sanitizer builds report.
 */
void a_translation_outlives_the_executor_that_built_it()
{
	constexpr unsigned long generation = 930;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	write_linked_pair(backing, target, 5);
	JitMapping mapping{generation, code_base, backing, 1};

	SharingEnabled sharing;
	{
		Memory memory{std::make_unique<StaticGuestMemory>()};
		auto producer = make_executor();
		arm64::CpuState cpu{};
		cpu.pc = target;
		require(run_once(*producer, *memory, generation, cpu).event ==
			::uml::cpu::EngineEvent::system_call);
		cpu = {};
		cpu.pc = code_base;
		const auto linked = run_once(*producer, *memory, generation, cpu);
		require(linked.event == ::uml::cpu::EngineEvent::system_call);
		require(linked.executed == 3);
	}

	Memory memory{std::make_unique<StaticGuestMemory>()};
	auto consumer = make_executor();
	arm64::CpuState cpu{};
	cpu.pc = target;
	require(run_once(*consumer, *memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	cpu = {};
	cpu.pc = code_base;
	const auto adopted = run_once(*consumer, *memory, generation, cpu);
	require(adopted.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 5);
	/* Three retired instructions means the branch went straight through into
	 * the successor: the relocated cell reached the consumer's own activated
	 * record, not the shared exit and not the producer's. */
	require(adopted.executed == 3);
	const auto diagnostics = consumer->cache_diagnostics();
	require(diagnostics.adoptions == 2);
	require(diagnostics.translations == 2);

	/* The store's own account of the same two translations: the producer
	 * published them and nothing displaced them, so both entries are still
	 * held and both were handed out exactly once. */
	const auto counters = gadgets::SharedProgramStore::instance().counters();
	require(counters.publications == 2);
	require(counters.adoptions == 2);
	require(counters.entries == 2);
	require(counters.evictions == 0);
	require(counters.bytes > 0);
}

/* Two address spaces running the same bytes at the same address are the same
 * translation, and a guest that runs one program in several processes is
 * exactly where that pays. Only the link cells are per-address-space, and those
 * are resolved by the consumer against whichever address space it is bound to.
 */
void one_address_space_adopts_another_s_translation()
{
	constexpr unsigned long producer_generation = 931;
	constexpr unsigned long consumer_generation = 932;
	constexpr auto target = code_base + 0x100;
	JitBacking producer_backing{1};
	JitBacking consumer_backing{1};
	write_linked_pair(producer_backing, target, 6);
	write_linked_pair(consumer_backing, target, 6);
	JitMapping producer_mapping{
		producer_generation, code_base, producer_backing, 1};
	JitMapping consumer_mapping{
		consumer_generation, code_base, consumer_backing, 1};

	SharingEnabled sharing;
	Memory producer_memory{std::make_unique<StaticGuestMemory>()};
	auto producer = make_executor();
	arm64::CpuState cpu{};
	cpu.pc = target;
	require(run_once(*producer, *producer_memory, producer_generation, cpu)
			.event == ::uml::cpu::EngineEvent::system_call);
	cpu = {};
	cpu.pc = code_base;
	require(run_once(*producer, *producer_memory, producer_generation, cpu)
			.event == ::uml::cpu::EngineEvent::system_call);

	Memory consumer_memory{std::make_unique<StaticGuestMemory>()};
	auto consumer = make_executor();
	cpu = {};
	cpu.pc = target;
	require(run_once(*consumer, *consumer_memory, consumer_generation, cpu)
			.event == ::uml::cpu::EngineEvent::system_call);
	cpu = {};
	cpu.pc = code_base;
	const auto adopted =
		run_once(*consumer, *consumer_memory, consumer_generation, cpu);
	require(adopted.event == ::uml::cpu::EngineEvent::system_call);
	require(adopted.executed == 3);
	require(cpu.registers[0] == 6);
	require(consumer->cache_diagnostics().adoptions == 2);
}

/* Gadget selection reads the code-generation profile, so two executors that do
 * not share one are not translating the same thing even from identical words.
 */
void a_different_code_generation_profile_does_not_share()
{
	constexpr unsigned long generation = 933;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	write_linked_pair(backing, target, 7);
	JitMapping mapping{generation, code_base, backing, 1};

	SharingEnabled sharing;
	Memory producer_memory{std::make_unique<StaticGuestMemory>()};
	auto producer = make_executor();
	arm64::CpuState cpu{};
	cpu.pc = target;
	require(run_once(*producer, *producer_memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);

	gadgets::BuildOptions software{};
	software.force_software_flags = true;
	Memory consumer_memory{std::make_unique<StaticGuestMemory>()};
	auto consumer = make_executor(software);
	cpu = {};
	cpu.pc = target;
	require(run_once(*consumer, *consumer_memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(consumer->cache_diagnostics().adoptions == 0);
	require(consumer->cache_diagnostics().translations == 1);
}

/* An adopted block is validated on the words it was adopted for, exactly as a
 * built one is. Nothing about having copied it excuses it from noticing that
 * the guest rewrote the code underneath it.
 */
void an_adopted_block_still_revalidates_its_words()
{
	constexpr unsigned long generation = 934;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	write_linked_pair(backing, target, 8);
	JitMapping mapping{generation, code_base, backing, 1};

	SharingEnabled sharing;
	Memory producer_memory{std::make_unique<StaticGuestMemory>()};
	auto producer = make_executor();
	arm64::CpuState cpu{};
	cpu.pc = target;
	require(run_once(*producer, *producer_memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);

	Memory consumer_memory{std::make_unique<StaticGuestMemory>()};
	auto consumer = make_executor();
	cpu = {};
	cpu.pc = target;
	require(run_once(*consumer, *consumer_memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 8);
	require(consumer->cache_diagnostics().adoptions == 1);

	write_mapped32(generation, target, move_immediate(0, 9));
	darwin_uml_static_vm_invalidate_code(target);
	cpu = {};
	cpu.pc = target;
	require(run_once(*consumer, *consumer_memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 9);
#if UML_GADGET_EXACT_CACHE_VALIDATION
	require(consumer->cache_diagnostics().validation_mismatches == 1);
#endif

	/* The address now holds different code, so the rebuild displaces the
	 * entry rather than being refused by it. A guest that regenerates code
	 * at one address -- which is what a JavaScript engine does all day --
	 * would otherwise never share any of it again. */
	const auto counters = gadgets::SharedProgramStore::instance().counters();
	require(counters.replacements == 1);
	require(counters.entries == 1);
}

} // namespace

int main()
{
	a_translation_outlives_the_executor_that_built_it();
	one_address_space_adopts_another_s_translation();
	a_different_code_generation_profile_does_not_share();
	an_adopted_block_still_revalidates_its_words();
	return 0;
}
