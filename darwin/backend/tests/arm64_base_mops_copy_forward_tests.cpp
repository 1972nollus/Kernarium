// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mops_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace mops = mops_test;
namespace engine = uml::backend::tests;

namespace {

constexpr unsigned destination_register = 3;
constexpr unsigned source_register = 4;
constexpr unsigned count_register = 5;

[[nodiscard]] constexpr std::array<engine::ProgramWord, 4>
sequence(unsigned options)
{
	return {{
		{.offset = 0,
		 .instruction = mops::copy_instruction(false,
			mops::Stage::prologue, options, destination_register,
			source_register, count_register)},
		{.offset = 4,
		 .instruction = mops::copy_instruction(false, mops::Stage::main,
			options, destination_register, source_register,
			count_register)},
		{.offset = 8,
		 .instruction = mops::copy_instruction(false,
			mops::Stage::epilogue, options, destination_register,
			source_register, count_register)},
		{.offset = 12, .instruction = mops::system_call},
	}};
}

void all_options_cross_pages(engine::EngineHarness &harness)
{
	const std::size_t size = mops::page_size + 257;
	const std::size_t source_offset = 123;
	const std::size_t destination_offset = 3 * mops::page_size + 311;
	for (unsigned options = 0; options < 16; ++options) {
		std::vector<std::byte> initial(6 * mops::page_size, std::byte{0xcc});
		mops::fill_pattern(std::span{initial}.subspan(source_offset, size),
			options + 7U);
		auto expected_memory = initial;
		mops::copy_bytes(expected_memory, destination_offset, source_offset,
			size);
		const auto source = mops::data_base + source_offset;
		const auto destination = mops::data_base + destination_offset;
		auto initial_cpu = mops::state();
		initial_cpu.registers[destination_register] = destination;
		initial_cpu.registers[source_register] = source;
		initial_cpu.registers[count_register] = size;
		auto expected_cpu = initial_cpu;
		expected_cpu.nzcv = mops::option_b_forward;
		expected_cpu.registers[destination_register] = destination + size;
		expected_cpu.registers[source_register] = source + size;
		expected_cpu.registers[count_register] = 0;
		expected_cpu.pc = mops::successful_program_pc(4);
		const std::array regions{engine::MemoryRegion{
			.address = mops::data_base,
			.initial = initial,
			.expected = expected_memory,
		}};
		const auto code = sequence(options);
		harness.check_program({
			.name = "CPYF option sequence across pages",
			.code = code,
			.initial = initial_cpu,
			.expected = expected_cpu,
			.native = mops::native(),
			.memory = regions,
		});
	}
}

void zero_and_maximum_lengths(engine::EngineHarness &harness)
{
	for (const auto requested : {
	     std::uint64_t{0x8000'0000'0000'0000ULL},
	     ~std::uint64_t{0},
	     mops::max_block}) {
		auto initial = mops::state();
		initial.registers[destination_register] = 0x1235;
		initial.registers[source_register] = 0x9877;
		initial.registers[count_register] = requested;
		auto expected = initial;
		expected.registers[count_register] = mops::max_block;
		expected.nzcv = mops::option_b_forward;
		harness.check_one("CPYF maximum length prologue",
			mops::copy_instruction(false, mops::Stage::prologue, 15,
				destination_register, source_register, count_register),
			initial, expected, mops::native());
	}

	auto initial = mops::state();
	initial.registers[destination_register] = 0x1235;
	initial.registers[source_register] = 0x9877;
	initial.registers[count_register] = 0;
	auto expected = initial;
	expected.nzcv = mops::option_b_forward;
	expected.pc = mops::successful_program_pc(4);
	const auto code = sequence(6);
	harness.check_program({
		.name = "CPYF zero length sequence",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = mops::native(),
	});
}

void forward_overlap_block_order(engine::EngineHarness &harness)
{
	/*
	 * MemCpyBytes reads at most MaxMOPSBlockSize (64) bytes before writing
	 * them. With this exact 64-byte displacement, both the byte-granular
	 * interpreter choice and the gadget's largest permitted block produce
	 * the same architecturally forward result. A page-sized bulk copy would
	 * incorrectly behave like an overlap-preserving general copy.
	 */
	constexpr std::size_t source_offset = 128;
	constexpr std::size_t displacement = 64;
	constexpr std::size_t size = 128;
	constexpr std::size_t destination_offset =
		source_offset + displacement;
	std::vector<std::byte> initial(mops::page_size);
	mops::fill_pattern(initial, 61);
	auto expected_memory = initial;
	for (std::size_t offset = 0; offset < size; ++offset)
		expected_memory[destination_offset + offset] =
			expected_memory[source_offset + offset];

	const auto source = mops::data_base + source_offset;
	const auto destination = mops::data_base + destination_offset;
	auto initial_cpu = mops::state();
	initial_cpu.registers[destination_register] = destination;
	initial_cpu.registers[source_register] = source;
	initial_cpu.registers[count_register] = size;
	auto expected_cpu = initial_cpu;
	expected_cpu.nzcv = mops::option_b_forward;
	expected_cpu.registers[destination_register] = destination + size;
	expected_cpu.registers[source_register] = source + size;
	expected_cpu.registers[count_register] = 0;
	expected_cpu.pc = mops::successful_program_pc(4);
	const std::array regions{engine::MemoryRegion{
		.address = mops::data_base,
		.initial = initial,
		.expected = expected_memory,
	}};
	const auto code = sequence(0);
	harness.check_program({
		.name = "CPYF forward overlap observes 64-byte block limit",
		.code = code,
		.initial = initial_cpu,
		.expected = expected_cpu,
		.native = mops::native(),
		.memory = regions,
	});
}

void malformed_stage_state(engine::EngineHarness &harness)
{
	const auto epilogue = mops::copy_instruction(false,
		mops::Stage::epilogue, 0, destination_register, source_register,
		count_register);
	for (const auto [remaining, nzcv] : {
	     std::pair{std::uint64_t{8}, 0U},
	     std::pair{mops::max_block + 1U, mops::option_b_forward}}) {
		auto initial = mops::state();
		initial.registers[destination_register] = mops::data_base;
		initial.registers[source_register] =
			mops::data_base + 2 * mops::page_size;
		initial.registers[count_register] = remaining;
		initial.nzcv = nzcv;
		mops::check_runtime_illegal(harness,
			"malformed CPYF epilogue state", epilogue, initial,
			mops::native());
	}

	for (unsigned options = 0; options < 16; ++options) {
		auto initial = mops::state();
		initial.registers[destination_register] = mops::data_base + 1;
		initial.registers[source_register] = mops::data_base + 3;
		initial.registers[count_register] = mops::max_block + 1U;
		initial.nzcv = 0;
		harness.check_one("zero-size CPYF main stage", mops::copy_instruction(
			false, mops::Stage::main, options, destination_register,
			source_register, count_register), initial, initial,
			mops::native());
	}

	auto zero = mops::state();
	zero.registers[destination_register] = 0x1003;
	zero.registers[source_register] = 0x2005;
	zero.registers[count_register] = 0;
	zero.nzcv = mops::option_b_backward;
	harness.check_one("CPYF zero epilogue ignores format state", epilogue,
		zero, zero, mops::native());
}

void non_format_flags_are_ignored(engine::EngineHarness &harness)
{
	constexpr std::size_t source_offset = 128;
	constexpr std::size_t destination_offset = 512;
	constexpr std::size_t size = 37;
	std::vector<std::byte> initial(mops::page_size, std::byte{0xcc});
	mops::fill_pattern(std::span{initial}.subspan(source_offset, size), 73);
	auto expected_memory = initial;
	mops::copy_bytes(expected_memory, destination_offset, source_offset, size);
	auto initial_cpu = mops::state();
	initial_cpu.registers[destination_register] =
		mops::data_base + destination_offset;
	initial_cpu.registers[source_register] =
		mops::data_base + source_offset;
	initial_cpu.registers[count_register] = size;
	initial_cpu.nzcv = 0xf000'0000U;
	auto expected_cpu = initial_cpu;
	expected_cpu.registers[destination_register] += size;
	expected_cpu.registers[source_register] += size;
	expected_cpu.registers[count_register] = 0;
	expected_cpu.pc = mops::successful_program_pc(2);
	const std::array code{
		engine::ProgramWord{
			.offset = 0,
			.instruction = mops::copy_instruction(false,
				mops::Stage::epilogue, 15, destination_register,
				source_register, count_register),
		},
		engine::ProgramWord{.offset = 4, .instruction = mops::system_call},
	};
	const std::array regions{engine::MemoryRegion{
		.address = mops::data_base,
		.initial = initial,
		.expected = expected_memory,
	}};
	harness.check_program({
		.name = "CPYF epilogue ignores N/Z/V format bits",
		.code = code,
		.initial = initial_cpu,
		.expected = expected_cpu,
		.native = mops::native(),
		.memory = regions,
	});
}

void read_fault_restart(engine::EngineHarness &harness)
{
	const auto source_page = mops::data_base;
	const auto destination_page = mops::data_base + 3 * mops::page_size;
	const auto source = source_page + mops::page_size - 32;
	const auto destination = destination_page + 79;
	std::vector<std::byte> source_initial(mops::page_size);
	mops::fill_pattern(source_initial, 31);
	std::vector<std::byte> destination_initial(mops::page_size,
		std::byte{0xcc});
	auto destination_expected = destination_initial;
	std::copy_n(source_initial.end() - 32, 32,
		destination_expected.begin() + 79);

	auto initial = mops::state();
	initial.registers[destination_register] = destination;
	initial.registers[source_register] = source;
	initial.registers[count_register] = 64;
	auto expected = initial;
	expected.nzcv = mops::option_b_forward;
	expected.registers[destination_register] = destination + 32;
	expected.registers[source_register] = source + 32;
	expected.registers[count_register] = 32;
	expected.pc = engine::engine_test_code_base + 8;
	const std::array regions{
		engine::MemoryRegion{
			.address = source_page,
			.initial = source_initial,
		},
		engine::MemoryRegion{
			.address = destination_page,
			.initial = destination_initial,
			.expected = destination_expected,
		},
	};
	const auto code = sequence(10);
	harness.check_program({
		.name = "CPYF read fault restart",
		.code = code,
		.initial = initial,
		.expected = expected,
		.event = engine::ExpectedEvent::memory_fault,
		.native = mops::native(),
		.fault = {
			.address = source + 32,
			.access = mops::arm64::MemoryAccess::read,
		},
		.compare_fault = true,
		.memory = regions,
	});
}

void write_permission_fault_restart(engine::EngineHarness &harness)
{
	const auto source_page = mops::data_base;
	const auto destination_page = mops::data_base + 3 * mops::page_size;
	const auto source = source_page + 101;
	const auto destination = destination_page + mops::page_size - 32;
	std::vector<std::byte> source_initial(mops::page_size);
	mops::fill_pattern(source_initial, 47);
	std::vector<std::byte> first_initial(mops::page_size, std::byte{0xcc});
	auto first_expected = first_initial;
	std::copy_n(source_initial.begin() + 101, 32,
		first_expected.end() - 32);
	const std::vector<std::byte> denied_initial(mops::page_size,
		std::byte{0xdd});

	auto initial = mops::state();
	initial.registers[destination_register] = destination;
	initial.registers[source_register] = source;
	initial.registers[count_register] = 64;
	auto expected = initial;
	expected.nzcv = mops::option_b_forward;
	expected.registers[destination_register] = destination + 32;
	expected.registers[source_register] = source + 32;
	expected.registers[count_register] = 32;
	expected.pc = engine::engine_test_code_base + 8;
	const std::array regions{
		engine::MemoryRegion{
			.address = source_page,
			.initial = source_initial,
		},
		engine::MemoryRegion{
			.address = destination_page,
			.initial = first_initial,
			.expected = first_expected,
		},
		engine::MemoryRegion{
			.address = destination_page + mops::page_size,
			.initial = denied_initial,
			.readable = true,
			.writable = false,
		},
	};
	const auto code = sequence(5);
	harness.check_program({
		.name = "CPYF write permission fault restart",
		.code = code,
		.initial = initial,
		.expected = expected,
		.event = engine::ExpectedEvent::memory_fault,
		.native = mops::native(),
		.fault = {
			.address = destination + 32,
			.access = mops::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = regions,
	});
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	all_options_cross_pages(harness);
	zero_and_maximum_lengths(harness);
	forward_overlap_block_order(harness);
	malformed_stage_state(harness);
	non_format_flags_are_ignored(harness);
	read_fault_restart(harness);
	write_permission_fault_restart(harness);
	harness.print_summary("arm64-base-mops-copy-forward");
}
