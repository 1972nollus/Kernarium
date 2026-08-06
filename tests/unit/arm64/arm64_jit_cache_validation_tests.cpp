// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

using namespace uml::emulator::tests;

namespace {

namespace arm64 = uml::cpu::arm64;
using uml::cpu::arm64::JitCacheValidationDiagnostics;
using uml::cpu::arm64::JitExecutor;
using uml::cpu::arm64::JitExecutorDiagnosticsOptions;
using uml::cpu::arm64::JitExecutorOptions;
using uml::memory::StaticGuestMemory;

constexpr std::uint32_t system_call = 0xd400'0001U;

[[nodiscard]] constexpr std::uint32_t
move_immediate(unsigned destination, unsigned immediate) noexcept
{
	return 0xd280'0000U | ((immediate & 0xffffU) << 5U) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
branch(std::uint64_t source, std::uint64_t target) noexcept
{
	const auto displacement = (static_cast<std::int64_t>(target) -
		static_cast<std::int64_t>(source)) / 4;
	return 0x1400'0000U |
		(static_cast<std::uint32_t>(displacement) & 0x03ff'ffffU);
}

struct Run final {
	::uml::cpu::EngineEvent event{};
	std::size_t executed{};
};

[[nodiscard]] Run run_once(JitExecutor &executor, StaticGuestMemory &memory,
	unsigned long generation, arm64::CpuState &cpu)
{
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto event = executor.run_block(cpu, memory, generation, executed);
	executor.end_execution();
	return {event, executed};
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

void unrelated_page_change_reuses_exact_block()
{
	constexpr unsigned long generation = 8101;
	constexpr std::uint64_t base = 0x90'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	JitBacking backing{2};
	backing.put32(0, move_immediate(0, 1));
	backing.put32(4, system_call);
	JitMapping mapping{generation, base, backing, 2};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = base;
	auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 1);
	require(executor.cache_validation_diagnostics().translations == 1);

	darwin_uml_static_vm_invalidate_code(base + page);
	cpu.pc = base;
	cpu.registers[0] = 0;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 1);
	const auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 1);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_hits == 1);
	require(diagnostics.validation_mismatches == 0);
	require(diagnostics.validation_faults == 0);
}

void in_block_change_rejects_saved_words()
{
	constexpr unsigned long generation = 8102;
	constexpr std::uint64_t base = 0x91'0000;
	JitBacking backing{1};
	backing.put32(0, move_immediate(0, 1));
	backing.put32(4, system_call);
	JitMapping mapping{generation, base, backing, 1};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	write_mapped32(generation, base, move_immediate(0, 2));
	darwin_uml_static_vm_invalidate_code(base);

	cpu.pc = base;
	cpu.registers[0] = 0;
	const auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 2);
	const auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 2);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_hits == 0);
	require(diagnostics.validation_mismatches == 1);
}

void executable_permission_change_rejects_saved_block()
{
	constexpr unsigned long generation = 8103;
	constexpr std::uint64_t base = 0x92'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	JitBacking backing{1};
	backing.put32(0, move_immediate(0, 3));
	backing.put32(4, system_call);
	JitMapping mapping{generation, base, backing, 1};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(darwin_uml_static_vm_protect(
		reinterpret_cast<void *>(base), page,
		PROT_READ | PROT_WRITE) == 0);

	cpu.pc = base;
	auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::memory_fault);
	auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 1);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_faults == 1);
	require(diagnostics.validation_hits == 0);

	require(darwin_uml_static_vm_protect(
		reinterpret_cast<void *>(base), page,
		PROT_READ | PROT_WRITE | PROT_EXEC) == 0);
	cpu.pc = base;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 2);
	require(diagnostics.validation_hits == 0);
}

void remapped_code_is_refetched_from_the_current_mapping()
{
	constexpr unsigned long generation = 8111;
	constexpr std::uint64_t base = 0x93'0000;
	JitBacking original{1};
	JitBacking replacement{1};
	original.put32(0, move_immediate(0, 3));
	original.put32(4, system_call);
	replacement.put32(0, move_immediate(0, 6));
	replacement.put32(4, system_call);
	JitMapping mapping{generation, base, original, 1};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 3);
	require(darwin_uml_static_vm_rebind(
		base, replacement.descriptor(), 0) == 0);

	cpu.pc = base;
	cpu.registers[0] = 0;
	const auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 6);
	const auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 2);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_mismatches == 1);
	require(diagnostics.validation_hits == 0);
}

void cross_page_block_is_validated_completely()
{
	constexpr unsigned long generation = 8104;
	constexpr std::uint64_t base = 0x94'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	const auto start = base + page - sizeof(std::uint32_t);
	JitBacking backing{3};
	backing.put32(page - sizeof(std::uint32_t), move_immediate(0, 4));
	backing.put32(page, system_call);
	JitMapping mapping{generation, base, backing, 3};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = start;
	auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 4);

	darwin_uml_static_vm_invalidate_code(base + 2 * page);
	cpu.pc = start;
	cpu.registers[0] = 0;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 4);
	const auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 1);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_hits == 1);

	/* The changed word is on the second executable page. */
	write_mapped32(generation, base + page, move_immediate(0, 5));
	write_mapped32(generation, base + page + 4U, system_call);
	darwin_uml_static_vm_invalidate_code(base + page);
	cpu.pc = start;
	cpu.registers[0] = 0;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 3);
	require(cpu.registers[0] == 5);
	const auto changed = executor.cache_validation_diagnostics();
	require(changed.translations == 2);
	require(changed.validation_attempts == 2);
	require(changed.validation_hits == 1);
	require(changed.validation_mismatches == 1);
}

void sequence_change_deactivates_unvalidated_successor_link()
{
	constexpr unsigned long generation = 8105;
	constexpr std::uint64_t base = 0x98'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	JitBacking backing{2};
	backing.put32(0, 0x1400'0002U); // b base + 8
	backing.put32(8, 0x9100'0400U); // add x0, x0, #1
	backing.put32(12, system_call);
	JitMapping mapping{generation, base, backing, 2};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = base;
	auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::continued);
	require(run.executed == 1);
	require(cpu.pc == base + 8);

	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 1);

	cpu.pc = base;
	cpu.registers[0] = 0;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 3);
	require(cpu.registers[0] == 1);

	darwin_uml_static_vm_invalidate_code(base + page);
	cpu.pc = base;
	cpu.registers[0] = 0;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::continued);
	require(run.executed == 1);
	require(cpu.pc == base + 8);
	require(cpu.registers[0] == 0);

	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 1);

	cpu.pc = base;
	cpu.registers[0] = 0;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 3);
	require(cpu.registers[0] == 1);
	const auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 2);
	require(diagnostics.validation_hits == 2);
}

[[nodiscard]] constexpr std::size_t cache_index(
	unsigned long generation, std::uint64_t pc) noexcept
{
	const auto mixed = pc ^ (pc >> 17U) ^
		(static_cast<std::uint64_t>(generation) * 0x9e37'79b9U);
	return static_cast<std::size_t>(mixed) & (8192U - 1U);
}

void cache_replacement_keeps_validation_metadata_bounded()
{
	constexpr unsigned long generation = 8106;
	constexpr std::uint64_t base = 0xa0'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::array<std::size_t, 5> offsets{
		0U, 0x2000U, 0x4000U, 0x6000U, 0x8000U};
	constexpr std::size_t pages = offsets.back() / page + 1U;
	static_assert(cache_index(generation, base + offsets[0]) ==
		cache_index(generation, base + offsets[1]));
	static_assert(cache_index(generation, base + offsets[0]) ==
		cache_index(generation, base + offsets[4]));

	JitBacking backing{pages};
	for (const auto offset : offsets)
		backing.put32(offset, system_call);
	JitMapping mapping{generation, base, backing, pages};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};
	for (const auto offset : offsets) {
		cpu.pc = base + offset;
		const auto run = run_once(executor, memory, generation, cpu);
		require(run.event == ::uml::cpu::EngineEvent::system_call);
		require(run.executed == 1);
	}
	auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == offsets.size());
	require(diagnostics.cache_replacements == 1);

	darwin_uml_static_vm_invalidate_code(base + page - 4U);
	cpu.pc = base + offsets.back();
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_hits == 1);

	const auto translations = diagnostics.translations;
	cpu.pc = base + offsets.front();
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.translations == translations + 1U);
}

void arena_reset_discards_saved_validation_words()
{
	constexpr unsigned long generation = 8107;
	constexpr std::uint64_t base = 0xa4'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	std::vector<std::uint32_t> instructions(page / sizeof(std::uint32_t),
		system_call);
	JitBacking backing{1};
	backing.put(0, std::as_bytes(std::span{instructions}));
	JitMapping mapping{generation, base, backing, 1};
	JitExecutor executor{
		JitExecutorOptions{},
		JitExecutorDiagnosticsOptions{
			.reuse_validated_stale_blocks = true,
			.code_arena_size = 16U * 1024U,
		}};
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	std::size_t compiled = 0;
	for (; compiled < instructions.size(); ++compiled) {
		cpu.pc = base + compiled * sizeof(std::uint32_t);
		require(run_once(executor, memory, generation, cpu).event ==
			::uml::cpu::EngineEvent::system_call);
		if (executor.cache_validation_diagnostics().arena_resets != 0)
			break;
	}
	require(compiled < instructions.size());
	auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.arena_resets == 1);
	const auto translations = diagnostics.translations;
	const auto attempts = diagnostics.validation_attempts;

	cpu.pc = base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == translations + 1U);
	require(diagnostics.validation_attempts == attempts);
}

void arena_reset_recompiles_link_descriptors()
{
	constexpr unsigned long generation = 8112;
	constexpr std::uint64_t base = 0xb4'0000;
	constexpr auto target =
		base + DARWIN_UML_GUEST_PAGE_SIZE - sizeof(std::uint32_t);
	constexpr std::size_t sources = 512;
	JitBacking backing{1};
	for (std::size_t index = 0; index < sources; ++index) {
		const auto source = base + index * sizeof(std::uint32_t);
		backing.put32(index * sizeof(std::uint32_t),
			branch(source, target));
	}
	backing.put32(target - base, system_call);
	JitMapping mapping{generation, base, backing, 1};
	JitExecutor executor{
		JitExecutorOptions{},
		JitExecutorDiagnosticsOptions{.code_arena_size = 4096}};
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = target;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	std::uint64_t reset_source = 0;
	for (std::size_t index = 0; index < sources; ++index) {
		cpu.pc = base + index * sizeof(std::uint32_t);
		const auto run = run_once(executor, memory, generation, cpu);
		if (executor.cache_validation_diagnostics().arena_resets != 0) {
			reset_source = base + index * sizeof(std::uint32_t);
			require(run.event == ::uml::cpu::EngineEvent::continued);
			require(cpu.pc == target);
			break;
		}
		require(run.event == ::uml::cpu::EngineEvent::system_call);
		require(run.executed == 2);
	}
	require(reset_source != 0);
	require(executor.cache_validation_diagnostics().arena_resets == 1);

	cpu.pc = target;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	cpu.pc = reset_source;
	const auto linked = run_once(executor, memory, generation, cpu);
	require(linked.event == ::uml::cpu::EngineEvent::system_call);
	require(linked.executed == 2);
}

void reordered_words_cannot_hide_behind_a_digest_collision()
{
	constexpr unsigned long generation = 8108;
	constexpr std::uint64_t base = 0xa8'0000;
	constexpr auto first = move_immediate(0, 1);
	constexpr std::uint32_t second = 0x9100'0800U; // add x0, x0, #2
	static_assert((first ^ second) == (second ^ first));
	static_assert(static_cast<std::uint32_t>(first + second) ==
		static_cast<std::uint32_t>(second + first));
	JitBacking backing{1};
	backing.put32(0, first);
	backing.put32(4, second);
	backing.put32(8, system_call);
	JitMapping mapping{generation, base, backing, 1};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 3);

	write_mapped32(generation, base, second);
	write_mapped32(generation, base + 4, first);
	darwin_uml_static_vm_invalidate_code(base);
	cpu.pc = base;
	cpu.registers[0] = 0;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 1);
	const auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 2);
	require(diagnostics.validation_mismatches == 1);
	require(diagnostics.validation_hits == 0);
}

struct Timing final {
	std::chrono::nanoseconds elapsed{};
	JitCacheValidationDiagnostics diagnostics{};
};

[[nodiscard]] Timing repeated_unrelated_changes(
	bool reuse, unsigned long generation, std::uint64_t base)
{
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::size_t iterations = 128;
	JitBacking backing{2};
	backing.put32(0, move_immediate(0, 7));
	backing.put32(4, system_call);
	JitMapping mapping{generation, base, backing, 2};
	JitExecutor executor{
		JitExecutorOptions{},
		JitExecutorDiagnosticsOptions{
			.reuse_validated_stale_blocks = reuse,
		}};
	StaticGuestMemory memory;
	arm64::CpuState cpu{};
	cpu.pc = base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);

	const auto start = std::chrono::steady_clock::now();
	for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
		darwin_uml_static_vm_invalidate_code(base + page);
		cpu.pc = base;
		require(run_once(executor, memory, generation, cpu).event ==
			::uml::cpu::EngineEvent::system_call);
	}
	const auto end = std::chrono::steady_clock::now();
	const auto diagnostics = executor.cache_validation_diagnostics();
	if (reuse) {
		require(diagnostics.translations == 1);
		require(diagnostics.validation_hits == iterations);
	} else {
		require(diagnostics.translations == iterations + 1U);
		require(diagnostics.validation_attempts == 0);
	}
	return {
		.elapsed =
			std::chrono::duration_cast<std::chrono::nanoseconds>(end - start),
		.diagnostics = diagnostics,
	};
}

void stale_sequence_control_reports_avoided_translation_work()
{
	const auto reused = repeated_unrelated_changes(true, 8109, 0xac'0000);
	const auto control = repeated_unrelated_changes(false, 8110, 0xb0'0000);
	std::fprintf(stderr,
		"JIT exact cache validation: reused=%llu ns/%llu translations "
		"control=%llu ns/%llu translations\n",
		static_cast<unsigned long long>(reused.elapsed.count()),
		static_cast<unsigned long long>(reused.diagnostics.translations),
		static_cast<unsigned long long>(control.elapsed.count()),
		static_cast<unsigned long long>(control.diagnostics.translations));
}

} // namespace

int main()
{
	unrelated_page_change_reuses_exact_block();
	in_block_change_rejects_saved_words();
	executable_permission_change_rejects_saved_block();
	remapped_code_is_refetched_from_the_current_mapping();
	cross_page_block_is_validated_completely();
	sequence_change_deactivates_unvalidated_successor_link();
	cache_replacement_keeps_validation_metadata_bounded();
	arena_reset_discards_saved_validation_words();
	arena_reset_recompiles_link_descriptors();
	reordered_words_cannot_hide_behind_a_digest_collision();
	stale_sequence_control_reports_avoided_translation_work();
	return 0;
}
