// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_lse128_pair_test_support.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <thread>

#include "uml_darwin_static_mmu.h"

namespace pair128 = arm64_base_lse128_pair_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

namespace {

constexpr std::array operations{
	pair128::Operation::clear,
	pair128::Operation::set,
	pair128::Operation::swap,
};

void native_store_pair(std::uint64_t *pointer, const pair128::Pair &value)
{
	std::uint64_t old_first;
	std::uint64_t old_second;
	std::uint32_t status;
	__asm__ volatile(
		"1:\n"
		"ldxp %0, %1, [%3]\n"
		"stlxp %w2, %4, %5, [%3]\n"
		"cbnz %w2, 1b"
		: "=&r"(old_first), "=&r"(old_second), "=&r"(status)
		: "r"(pointer), "r"(value[0]), "r"(value[1])
		: "memory");
	(void)old_first;
	(void)old_second;
}

[[nodiscard]] pair128::Pair native_load_pair(const std::uint64_t *pointer)
{
	pair128::Pair value{};
	__asm__ volatile(
		"ldxp %0, %1, [%2]\n"
		"clrex"
		: "=&r"(value[0]), "=&r"(value[1])
		: "r"(pointer)
		: "memory");
	return value;
}

[[nodiscard]] std::uint64_t *translate_pair(
	unsigned long generation, darwin_uml_static_access access)
{
	void *translated = nullptr;
	pair128::require(
		darwin_uml_static_vm_translate(generation, atomic::data_address,
			access, &translated) == DARWIN_UML_STATIC_TRANSLATED &&
			translated != nullptr,
		"cannot translate concurrent pair backing");
	return static_cast<std::uint64_t *>(translated);
}

struct ConcurrentPair final {
	std::thread worker;
	std::atomic<bool> running{};
	std::atomic<bool> ready{};
	std::atomic<bool> torn{};
	std::uint64_t *host{};
};

void start_writer(void *opaque, std::string_view, unsigned long generation)
{
	auto &context = *static_cast<ConcurrentPair *>(opaque);
	pair128::require(!context.worker.joinable(),
		"concurrent writer was already active");
	context.host = translate_pair(generation, DARWIN_UML_STATIC_WRITE);
	context.running.store(true, std::memory_order_relaxed);
	context.ready.store(false, std::memory_order_relaxed);
	context.worker = std::thread{[&context] {
		native_store_pair(context.host, pair128::operand);
		context.ready.store(true, std::memory_order_release);
		while (context.running.load(std::memory_order_acquire)) {
			native_store_pair(context.host, pair128::observed);
			native_store_pair(context.host, pair128::operand);
		}
	}};
	while (!context.ready.load(std::memory_order_acquire))
		std::this_thread::yield();
}

void stop_writer(void *opaque, std::string_view, unsigned long)
{
	auto &context = *static_cast<ConcurrentPair *>(opaque);
	context.running.store(false, std::memory_order_release);
	context.worker.join();
	native_store_pair(context.host, pair128::observed);
	context.host = nullptr;
}

void start_reader(void *opaque, std::string_view, unsigned long generation)
{
	auto &context = *static_cast<ConcurrentPair *>(opaque);
	pair128::require(!context.worker.joinable(),
		"concurrent reader was already active");
	context.host = translate_pair(generation, DARWIN_UML_STATIC_READ);
	context.running.store(true, std::memory_order_relaxed);
	context.ready.store(false, std::memory_order_relaxed);
	context.torn.store(false, std::memory_order_relaxed);
	context.worker = std::thread{[&context] {
		do {
			const auto value = native_load_pair(context.host);
			if (value != pair128::observed && value != pair128::operand)
				context.torn.store(true, std::memory_order_relaxed);
			context.ready.store(true, std::memory_order_release);
		} while (context.running.load(std::memory_order_acquire));
	}};
	while (!context.ready.load(std::memory_order_acquire))
		std::this_thread::yield();
}

void stop_reader(void *opaque, std::string_view, unsigned long)
{
	auto &context = *static_cast<ConcurrentPair *>(opaque);
	context.running.store(false, std::memory_order_release);
	context.worker.join();
	pair128::require(!context.torn.load(std::memory_order_relaxed),
		"concurrent reader observed a torn pair");
	context.host = nullptr;
}

void validate_complete_return(
	const pair128::arm64::CpuState &,
	const pair128::arm64::CpuState &expected,
	const pair128::arm64::CpuState &actual, std::string_view name,
	std::string_view engine_name, std::uint32_t instruction)
{
	const auto returned = pair128::read_pair(actual, 3, 4);
	pair128::require(returned == pair128::observed ||
			returned == pair128::operand,
		"atomic operation returned a torn pair");
	auto normalized = actual;
	pair128::write_pair(normalized, 3, 4,
		pair128::read_pair(expected, 3, 4));
	engine::require_architecture_equal(
		expected, normalized, name, engine_name, instruction);
}

void operation_order_and_stack_matrix(
	engine::EngineHarness &harness, pair128::ExpectedCounts &counts)
{
	for (const auto operation : operations) {
		for (const bool acquire : {false, true}) {
			for (const bool release : {false, true}) {
				for (const unsigned rn : {2U, 31U}) {
					auto initial = atomic::state(
						100U + pair128::code(operation) * 16U +
						static_cast<unsigned>(acquire) * 4U +
						static_cast<unsigned>(release) * 2U +
						static_cast<unsigned>(rn == 31U));
					pair128::write_pair(initial, 3, 4, pair128::operand);
					atomic::write_register(initial, rn,
						atomic::data_address, true);
					char name[128];
					std::snprintf(name, sizeof(name),
						"operation=%u acquire=%u release=%u base=%s",
						pair128::code(operation),
						static_cast<unsigned>(acquire),
						static_cast<unsigned>(release),
						rn == 31U ? "sp" : "x2");
					pair128::check(harness, counts, name, operation, acquire,
						release, rn, 3, 4, initial);
				}
			}
		}
	}
}

void every_distinct_transfer_pair(
	engine::EngineHarness &harness, pair128::ExpectedCounts &counts)
{
	for (const auto operation : operations) {
		for (unsigned first = 0; first < 31; ++first) {
			for (unsigned second = 0; second < 31; ++second) {
				if (first == second)
					continue;
				const bool acquire = (first & 1U) != 0U;
				const bool release = (second & 1U) != 0U;
				auto initial = atomic::state(
					1000U + pair128::code(operation) * 1024U +
					first * 32U + second);
				const pair128::Pair source{
					pair128::operand[0] ^
						(static_cast<std::uint64_t>(first) << 40),
					pair128::operand[1] ^
						(static_cast<std::uint64_t>(second) << 36),
				};
				pair128::write_pair(initial, first, second, source);
				initial.sp = atomic::data_address;
				char name[128];
				std::snprintf(name, sizeof(name),
					"operation=%u every transfer pair rt=%u rt2=%u",
					pair128::code(operation), first, second);
				pair128::check(harness, counts, name, operation, acquire,
					release, 31, first, second, initial);
			}
		}
	}
}

void every_base_and_base_transfer_alias(
	engine::EngineHarness &harness, pair128::ExpectedCounts &counts)
{
	for (const auto operation : operations) {
		for (unsigned rn = 0; rn < 32; ++rn) {
			auto initial = atomic::state(
				5000U + pair128::code(operation) * 64U + rn);
			pair128::write_pair(initial, 3, 4, pair128::operand);
			/*
			 * Base is resolved after the operands. For rn==rt/rt2 this
			 * deliberately verifies that the original address participates
			 * in the atomic operation before the old memory pair is returned.
			 */
			atomic::write_register(initial, rn, atomic::data_address, true);
			char name[128];
			std::snprintf(name, sizeof(name),
				"operation=%u every base rn=%u", pair128::code(operation),
				rn);
			pair128::check(harness, counts, name, operation,
				(rn & 1U) != 0U, (rn & 2U) != 0U, rn, 3, 4, initial);
		}
	}
}

void single_copy_atomicity(
	engine::EngineHarness &harness, pair128::ExpectedCounts &counts)
{
	atomic::Page page{};
	pair128::put_pair(page, atomic::data_offset, pair128::observed);
	const auto memory = atomic::region(page, page);
	const auto native = atomic::lse128();

	ConcurrentPair writer;
	auto initial = atomic::state(0x5800);
	initial.registers[2] = atomic::data_address;
	pair128::write_pair(initial, 3, 4, {});
	auto expected = initial;
	pair128::write_pair(expected, 3, 4, pair128::observed);
	harness.check({
		.name = "concurrent writer yields one complete returned pair",
		.instruction = pair128::instruction(
			pair128::Operation::clear, true, true, 2, 3, 4),
		.initial = initial,
		.expected = expected,
		.native = native,
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
		.validator = validate_complete_return,
		.validator_relation_id = "concurrent-complete-pair-return/v1",
		.execution_context = &writer,
		.before_engine = start_writer,
		.after_engine = stop_writer,
	});
	counts.add(native);

	constexpr std::size_t swaps = 62;
	std::array<engine::ProgramWord, swaps + 1> code{};
	for (std::size_t index = 0; index < swaps; ++index)
		code[index] = {
			.offset = index * sizeof(std::uint32_t),
			.instruction = pair128::instruction(
				pair128::Operation::swap, true, true, 2, 3, 4),
		};
	code.back() = {
		.offset = swaps * sizeof(std::uint32_t),
		.instruction = atomic::terminal_system_call,
	};
	ConcurrentPair reader;
	initial = atomic::state(0x5900);
	initial.registers[2] = atomic::data_address;
	pair128::write_pair(initial, 3, 4, pair128::operand);
	expected = initial;
	expected.pc = engine::engine_test_code_base +
		code.size() * sizeof(std::uint32_t);
	harness.check_program({
		.name = "concurrent reader sees only complete pair stores",
		.code = std::span<const engine::ProgramWord>{code},
		.initial = initial,
		.expected = expected,
		.native = native,
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
		.execution_context = &reader,
		.before_engine = start_reader,
		.after_engine = stop_reader,
	});
	counts.add(native);
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	pair128::ExpectedCounts counts;
	operation_order_and_stack_matrix(harness, counts);
	every_distinct_transfer_pair(harness, counts);
	every_base_and_base_transfer_alias(harness, counts);
	single_copy_atomicity(harness, counts);
	pair128::require_counts(harness, counts);
	harness.print_summary("arm64-base-lse128-pair-behavior");
}
