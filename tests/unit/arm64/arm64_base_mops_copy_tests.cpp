// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mops_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace mops = mops_test;
namespace engine = uml::emulator::tests;

namespace {

constexpr unsigned destination_register = 3;
constexpr unsigned source_register = 4;
constexpr unsigned count_register = 5;

[[nodiscard]] constexpr std::array<engine::ProgramWord, 4>
sequence(unsigned options)
{
	return {{
		{.offset = 0,
		 .instruction = mops::copy_instruction(true,
			mops::Stage::prologue, options, destination_register,
			source_register, count_register)},
		{.offset = 4,
		 .instruction = mops::copy_instruction(true, mops::Stage::main,
			options, destination_register, source_register,
			count_register)},
		{.offset = 8,
		 .instruction = mops::copy_instruction(true,
			mops::Stage::epilogue, options, destination_register,
			source_register, count_register)},
		{.offset = 12, .instruction = mops::system_call},
	}};
}

void all_options_overlap_pages(engine::EngineHarness &harness)
{
	const std::size_t lower_offset = mops::page_size - 211;
	const std::size_t delta = 173;
	const std::size_t size = mops::page_size + 257;
	for (unsigned options = 0; options < 16; ++options) {
		const bool backward = (options & 1U) == 0;
		const std::size_t source_offset =
			lower_offset + (backward ? 0 : delta);
		const std::size_t destination_offset =
			lower_offset + (backward ? delta : 0);
		std::vector<std::byte> initial(4 * mops::page_size);
		mops::fill_pattern(initial, options + 23U);
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
		expected_cpu.nzcv = backward ? mops::option_b_backward :
			mops::option_b_forward;
		expected_cpu.registers[count_register] = 0;
		if (!backward) {
			expected_cpu.registers[destination_register] += size;
			expected_cpu.registers[source_register] += size;
		}
		expected_cpu.pc = mops::successful_program_pc(4);
		const std::array regions{engine::MemoryRegion{
			.address = mops::data_base,
			.initial = initial,
			.expected = expected_memory,
		}};
		const auto code = sequence(options);
		harness.check_program({
			.name = "CPY option overlap sequence across pages",
			.code = code,
			.initial = initial_cpu,
			.expected = expected_cpu,
			.native = mops::native(),
			.memory = regions,
		});
	}
}

void direction_selection(engine::EngineHarness &harness)
{
	struct DirectionCase final {
		std::uint64_t source;
		std::uint64_t destination;
		std::uint64_t size;
		bool backward;
	};
	constexpr std::array cases{
		DirectionCase{0x1000, 0x1008, 32, true},
		DirectionCase{0x1008, 0x1000, 32, false},
		DirectionCase{0x1000, 0x2000, 32, false},
		DirectionCase{0x2000, 0x1000, 32, false},
		DirectionCase{0x1000, 0x1000, 32, false},
		DirectionCase{0x0b00'0000'0000'1000ULL,
			0x0300'0000'0000'1008ULL, 32, true},
	};
	for (const auto &item : cases) {
		auto initial = mops::state();
		initial.registers[destination_register] = item.destination;
		initial.registers[source_register] = item.source;
		initial.registers[count_register] = item.size;
		auto expected = initial;
		expected.nzcv = item.backward ? mops::option_b_backward :
			mops::option_b_forward;
		if (item.backward) {
			expected.registers[destination_register] += item.size;
			expected.registers[source_register] += item.size;
		}
		harness.check_one("CPY direction selection",
			mops::copy_instruction(true, mops::Stage::prologue, 0,
				destination_register, source_register, count_register),
			initial, expected, mops::native());
	}
}

void zero_maximum_and_malformed_state(engine::EngineHarness &harness)
{
	for (const auto requested : {
	     mops::max_copy,
	     mops::max_copy + 1U,
	     ~std::uint64_t{0}}) {
		auto initial = mops::state();
		initial.registers[destination_register] = 0x1000;
		initial.registers[source_register] = 0x2000;
		initial.registers[count_register] = requested;
		auto expected = initial;
		expected.registers[count_register] = mops::max_copy;
		expected.nzcv = mops::option_b_forward;
		harness.check_one("CPY maximum length prologue",
			mops::copy_instruction(true, mops::Stage::prologue, 9,
				destination_register, source_register, count_register),
			initial, expected, mops::native());
	}

	const auto epilogue = mops::copy_instruction(true,
		mops::Stage::epilogue, 0, destination_register, source_register,
		count_register);
	for (const auto [remaining, nzcv] : {
	     std::pair{std::uint64_t{8}, 0U},
	     std::pair{mops::max_copy + 1U, mops::option_b_forward},
	     std::pair{mops::max_copy + 1U, mops::option_b_backward}}) {
		auto initial = mops::state();
		initial.registers[destination_register] = mops::data_base;
		initial.registers[source_register] =
			mops::data_base + 2 * mops::page_size;
		initial.registers[count_register] = remaining;
		initial.nzcv = nzcv;
		mops::check_runtime_illegal(harness,
			"malformed CPY epilogue state", epilogue, initial,
			mops::native());
	}

	for (unsigned options = 0; options < 16; ++options) {
		auto initial = mops::state();
		initial.registers[destination_register] = mops::data_base + 1;
		initial.registers[source_register] = mops::data_base + 3;
		initial.registers[count_register] = mops::max_copy + 1U;
		initial.nzcv = 0;
		harness.check_one("zero-size CPY main stage", mops::copy_instruction(
			true, mops::Stage::main, options, destination_register,
			source_register, count_register), initial, initial,
			mops::native());
	}

	auto zero = mops::state();
	zero.registers[destination_register] = 0x1003;
	zero.registers[source_register] = 0x2005;
	zero.registers[count_register] = 0;
	zero.nzcv = 0;
	harness.check_one("CPY zero epilogue ignores format state", epilogue,
		zero, zero, mops::native());
}

void forward_read_fault_restart(engine::EngineHarness &harness)
{
	const auto source_page = mops::data_base;
	const auto destination_page = mops::data_base + 3 * mops::page_size;
	const auto source = source_page + mops::page_size - 32;
	const auto destination = destination_page + 79;
	std::vector<std::byte> source_initial(mops::page_size);
	mops::fill_pattern(source_initial, 67);
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
	const auto code = sequence(12);
	harness.check_program({
		.name = "forward CPY read fault restart",
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

void backward_read_fault_restart(engine::EngineHarness &harness)
{
	const auto source_page = mops::data_base + 2 * mops::page_size;
	const auto destination_page = mops::data_base + 5 * mops::page_size;
	std::vector<std::byte> source_initial(mops::page_size);
	mops::fill_pattern(source_initial, 37);
	std::vector<std::byte> destination_initial(mops::page_size,
		std::byte{0xcc});
	auto destination_expected = destination_initial;
	std::copy_n(source_initial.begin(), 32, destination_expected.begin());
	auto initial = mops::state();
	initial.registers[destination_register] = destination_page + 32;
	initial.registers[source_register] = source_page + 32;
	initial.registers[count_register] = 64;
	initial.nzcv = mops::option_b_backward;
	auto expected = initial;
	expected.registers[destination_register] = destination_page;
	expected.registers[source_register] = source_page;
	expected.registers[count_register] = 32;
	expected.pc = engine::engine_test_code_base;
	const std::array code{
		engine::ProgramWord{.offset = 0, .instruction =
			mops::copy_instruction(true, mops::Stage::epilogue, 8,
				destination_register, source_register, count_register)},
		engine::ProgramWord{.offset = 4, .instruction = mops::system_call},
	};
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
	harness.check_program({
		.name = "backward CPY read fault restart",
		.code = code,
		.initial = initial,
		.expected = expected,
		.event = engine::ExpectedEvent::memory_fault,
		.native = mops::native(),
		.fault = {
			.address = source_page - 1,
			.access = mops::arm64::MemoryAccess::read,
		},
		.compare_fault = true,
		.memory = regions,
	});
}

void backward_write_permission_fault_restart(engine::EngineHarness &harness)
{
	const auto source_page = mops::data_base + 2 * mops::page_size;
	const auto denied_page = mops::data_base + 4 * mops::page_size;
	const auto destination_page = denied_page + mops::page_size;
	std::vector<std::byte> source_initial(mops::page_size);
	mops::fill_pattern(source_initial, 53);
	const std::vector<std::byte> denied_initial(mops::page_size,
		std::byte{0xdd});
	std::vector<std::byte> destination_initial(mops::page_size,
		std::byte{0xcc});
	auto destination_expected = destination_initial;
	std::copy_n(source_initial.begin() + 32, 32,
		destination_expected.begin());
	auto initial = mops::state();
	initial.registers[destination_register] = destination_page + 32;
	initial.registers[source_register] = source_page + 64;
	initial.registers[count_register] = 64;
	initial.nzcv = mops::option_b_backward;
	auto expected = initial;
	expected.registers[destination_register] = destination_page;
	expected.registers[source_register] = source_page + 32;
	expected.registers[count_register] = 32;
	expected.pc = engine::engine_test_code_base;
	const std::array code{
		engine::ProgramWord{.offset = 0, .instruction =
			mops::copy_instruction(true, mops::Stage::epilogue, 7,
				destination_register, source_register, count_register)},
		engine::ProgramWord{.offset = 4, .instruction = mops::system_call},
	};
	const std::array regions{
		engine::MemoryRegion{
			.address = source_page,
			.initial = source_initial,
		},
		engine::MemoryRegion{
			.address = denied_page,
			.initial = denied_initial,
			.readable = true,
			.writable = false,
		},
		engine::MemoryRegion{
			.address = destination_page,
			.initial = destination_initial,
			.expected = destination_expected,
		},
	};
	harness.check_program({
		.name = "backward CPY write permission fault restart",
		.code = code,
		.initial = initial,
		.expected = expected,
		.event = engine::ExpectedEvent::memory_fault,
		.native = mops::native(),
		.fault = {
			.address = destination_page - 1,
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
	all_options_overlap_pages(harness);
	direction_selection(harness);
	zero_maximum_and_malformed_state(harness);
	forward_read_fault_restart(harness);
	backward_read_fault_restart(harness);
	backward_write_permission_fault_restart(harness);
	harness.print_summary("arm64-base-mops-copy");
}
