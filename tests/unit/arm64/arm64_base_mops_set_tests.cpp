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
constexpr unsigned count_register = 4;
constexpr unsigned source_register = 5;

[[nodiscard]] constexpr std::array<engine::ProgramWord, 4>
sequence(unsigned options, unsigned source)
{
	return {{
		{.offset = 0,
		 .instruction = mops::set_instruction(mops::Stage::prologue,
			options, destination_register, count_register, source)},
		{.offset = 4,
		 .instruction = mops::set_instruction(mops::Stage::main,
			options, destination_register, count_register, source)},
		{.offset = 8,
		 .instruction = mops::set_instruction(mops::Stage::epilogue,
			options, destination_register, count_register, source)},
		{.offset = 12, .instruction = mops::system_call},
	}};
}

void all_options_sources_and_pages(engine::EngineHarness &harness)
{
	const std::size_t destination_offset = mops::page_size - 113;
	const std::size_t size = mops::page_size + 257;
	for (unsigned options = 0; options < 4; ++options)
		for (const unsigned source : {source_register, 31U}) {
			std::vector<std::byte> initial(3 * mops::page_size,
				std::byte{0xcc});
			auto expected_memory = initial;
			const auto fill = source == 31U ? std::byte{} :
				static_cast<std::byte>(0x70U + options);
			mops::set_bytes(expected_memory, destination_offset, size,
				fill);
			const auto destination =
				mops::data_base + destination_offset;
			auto initial_cpu = mops::state();
			initial_cpu.registers[destination_register] = destination;
			initial_cpu.registers[count_register] = size;
			initial_cpu.registers[source_register] =
				static_cast<unsigned>(fill);
			auto expected_cpu = initial_cpu;
			expected_cpu.nzcv = mops::option_b_forward;
			expected_cpu.registers[destination_register] =
				destination + size;
			expected_cpu.registers[count_register] = 0;
			expected_cpu.pc = mops::successful_program_pc(4);
			const std::array regions{engine::MemoryRegion{
				.address = mops::data_base,
				.initial = initial,
				.expected = expected_memory,
			}};
			const auto code = sequence(options, source);
			harness.check_program({
				.name = "SET option/source sequence across pages",
				.code = code,
				.initial = initial_cpu,
				.expected = expected_cpu,
				.native = mops::native(),
				.memory = regions,
			});
		}
}

void zero_maximum_and_malformed_state(engine::EngineHarness &harness)
{
	for (const auto requested : {
	     mops::max_block,
	     std::uint64_t{0x8000'0000'0000'0000ULL},
	     ~std::uint64_t{0}}) {
		auto initial = mops::state();
		initial.registers[destination_register] = 0x1003;
		initial.registers[count_register] = requested;
		initial.registers[source_register] = 0x5a;
		auto expected = initial;
		expected.registers[count_register] = mops::max_block;
		expected.nzcv = mops::option_b_forward;
		harness.check_one("SET maximum length prologue",
			mops::set_instruction(mops::Stage::prologue, 3,
				destination_register, count_register, source_register),
			initial, expected, mops::native());
	}

	auto zero_initial = mops::state();
	zero_initial.registers[destination_register] = 0x1235;
	zero_initial.registers[count_register] = 0;
	zero_initial.registers[source_register] = 0xa5;
	auto zero_expected = zero_initial;
	zero_expected.nzcv = mops::option_b_forward;
	zero_expected.pc = mops::successful_program_pc(4);
	const auto zero_code = sequence(2, source_register);
	harness.check_program({
		.name = "SET zero length sequence",
		.code = zero_code,
		.initial = zero_initial,
		.expected = zero_expected,
		.native = mops::native(),
	});

	const auto epilogue = mops::set_instruction(mops::Stage::epilogue, 0,
		destination_register, count_register, source_register);
	for (const auto [remaining, nzcv] : {
	     std::pair{std::uint64_t{8}, 0U},
	     std::pair{mops::max_block + 1U, mops::option_b_forward}}) {
		auto initial = mops::state();
		initial.registers[destination_register] = mops::data_base;
		initial.registers[count_register] = remaining;
		initial.registers[source_register] = 0x5a;
		initial.nzcv = nzcv;
		mops::check_runtime_illegal(harness,
			"malformed SET epilogue state", epilogue, initial,
			mops::native());
	}

	for (unsigned options = 0; options < 4; ++options) {
		auto initial = mops::state();
		initial.registers[destination_register] = mops::data_base + 1;
		initial.registers[count_register] = mops::max_block + 1U;
		initial.registers[source_register] = 0x6d;
		initial.nzcv = 0;
		harness.check_one("zero-size SET main stage",
			mops::set_instruction(mops::Stage::main, options,
				destination_register, count_register, source_register),
			initial, initial, mops::native());
	}

	auto zero = mops::state();
	zero.registers[destination_register] = 0x1003;
	zero.registers[count_register] = 0;
	zero.registers[source_register] = 0x5a;
	zero.nzcv = mops::option_b_backward;
	harness.check_one("SET zero epilogue ignores format state", epilogue,
		zero, zero, mops::native());
}

void non_format_flags_are_ignored(engine::EngineHarness &harness)
{
	constexpr std::size_t destination_offset = 317;
	constexpr std::size_t size = 37;
	std::vector<std::byte> initial(mops::page_size, std::byte{0xcc});
	auto expected_memory = initial;
	mops::set_bytes(expected_memory, destination_offset, size,
		std::byte{0x6d});
	auto initial_cpu = mops::state();
	initial_cpu.registers[destination_register] =
		mops::data_base + destination_offset;
	initial_cpu.registers[count_register] = size;
	initial_cpu.registers[source_register] = 0x6d;
	initial_cpu.nzcv = 0xf000'0000U;
	auto expected_cpu = initial_cpu;
	expected_cpu.registers[destination_register] += size;
	expected_cpu.registers[count_register] = 0;
	expected_cpu.pc = mops::successful_program_pc(2);
	const std::array code{
		engine::ProgramWord{
			.offset = 0,
			.instruction = mops::set_instruction(
				mops::Stage::epilogue, 3, destination_register,
				count_register, source_register),
		},
		engine::ProgramWord{.offset = 4, .instruction = mops::system_call},
	};
	const std::array regions{engine::MemoryRegion{
		.address = mops::data_base,
		.initial = initial,
		.expected = expected_memory,
	}};
	harness.check_program({
		.name = "SET epilogue ignores N/Z/V format bits",
		.code = code,
		.initial = initial_cpu,
		.expected = expected_cpu,
		.native = mops::native(),
		.memory = regions,
	});
}

void write_permission_fault_restart(engine::EngineHarness &harness)
{
	const auto writable_page = mops::data_base + mops::page_size;
	const auto denied_page = writable_page + mops::page_size;
	const auto destination = writable_page + mops::page_size - 37;
	std::vector<std::byte> writable_initial(mops::page_size,
		std::byte{0xcc});
	auto writable_expected = writable_initial;
	std::fill_n(writable_expected.end() - 37, 37, std::byte{0x6d});
	const std::vector<std::byte> denied_initial(mops::page_size,
		std::byte{0xdd});
	auto initial = mops::state();
	initial.registers[destination_register] = destination;
	initial.registers[count_register] = 64;
	initial.registers[source_register] = 0x6d;
	auto expected = initial;
	expected.nzcv = mops::option_b_forward;
	expected.registers[destination_register] = destination + 37;
	expected.registers[count_register] = 27;
	expected.pc = engine::engine_test_code_base + 8;
	const std::array regions{
		engine::MemoryRegion{
			.address = writable_page,
			.initial = writable_initial,
			.expected = writable_expected,
		},
		engine::MemoryRegion{
			.address = denied_page,
			.initial = denied_initial,
			.readable = true,
			.writable = false,
		},
	};
	const auto code = sequence(3, source_register);
	harness.check_program({
		.name = "SET write permission fault restart",
		.code = code,
		.initial = initial,
		.expected = expected,
		.event = engine::ExpectedEvent::memory_fault,
		.native = mops::native(),
		.fault = {
			.address = denied_page,
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
	all_options_sources_and_pages(harness);
	zero_maximum_and_malformed_state(harness);
	non_format_flags_are_ignored(harness);
	write_permission_fault_restart(harness);
	harness.print_summary("arm64-base-mops-set");
}
