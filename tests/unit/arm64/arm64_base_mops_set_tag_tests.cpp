// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mops_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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
		 .instruction = mops::set_tag_instruction(mops::Stage::prologue,
			options, destination_register, count_register, source)},
		{.offset = 4,
		 .instruction = mops::set_tag_instruction(mops::Stage::main,
			options, destination_register, count_register, source)},
		{.offset = 8,
		 .instruction = mops::set_tag_instruction(mops::Stage::epilogue,
			options, destination_register, count_register, source)},
		{.offset = 12, .instruction = mops::system_call},
	}};
}

void all_options_sources_pages_and_tags(engine::EngineHarness &harness)
{
	const std::size_t destination_offset = mops::page_size - 32;
	const std::size_t size = mops::page_size + 64;
	for (unsigned options = 0; options < 4; ++options)
		for (const unsigned source : {source_register, 31U}) {
			const unsigned logical_tag = options + 7U;
			const auto fill = source == 31U ? std::byte{} :
				static_cast<std::byte>(0x70U + options);
			std::vector<std::byte> initial(3 * mops::page_size,
				std::byte{0xcc});
			auto expected_memory = initial;
			mops::set_bytes(expected_memory, destination_offset, size,
				fill);
			const std::array initial_tags{
				engine::AllocationTag{.offset = 0, .value = 2},
				engine::AllocationTag{
					.offset = destination_offset,
					.value = 1,
				},
			};
			std::vector<engine::AllocationTag> expected_tags;
			expected_tags.reserve(size / 16);
			for (std::size_t offset = 0; offset < size; offset += 16)
				expected_tags.push_back({
					.offset = destination_offset + offset,
					.value = static_cast<std::uint8_t>(logical_tag),
				});
			const auto destination = mops::tagged_address(
				mops::data_base + destination_offset, logical_tag);
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
				.initial_tags = initial_tags,
				.expected_tags = expected_tags,
			}};
			const auto code = sequence(options, source);
			harness.check_program({
				.name = "SETG option/source sequence across pages",
				.code = code,
				.initial = initial_cpu,
				.expected = expected_cpu,
				.native = mops::native(true),
				.memory = regions,
			});
		}
}

void zero_maximum_and_main_stage(engine::EngineHarness &harness)
{
	for (const auto requested : {
	     mops::max_tag_set,
	     std::uint64_t{0x8000'0000'0000'0001ULL},
	     ~std::uint64_t{0}}) {
		auto initial = mops::state();
		initial.registers[destination_register] =
			mops::tagged_address(0x1000, 11);
		initial.registers[count_register] = requested;
		initial.registers[source_register] = 0x5a;
		auto expected = initial;
		expected.registers[count_register] = mops::max_tag_set;
		expected.nzcv = mops::option_b_forward;
		harness.check_one("SETG maximum length prologue",
			mops::set_tag_instruction(mops::Stage::prologue, 3,
				destination_register, count_register, source_register),
			initial, expected, mops::native(true));
	}

	auto just_over = mops::state();
	just_over.registers[destination_register] =
		mops::tagged_address(0x1000, 11);
	just_over.registers[count_register] = mops::max_tag_set + 1U;
	just_over.registers[source_register] = 0x5a;
	harness.check({
		.name = "SETG just-over-limit remains an unaligned positive size",
		.instruction = mops::set_tag_instruction(
			mops::Stage::prologue, 3, destination_register,
			count_register, source_register),
		.initial = just_over,
		.expected = just_over,
		.event = engine::ExpectedEvent::memory_fault,
		.native = mops::native(true),
		.fault = {
			.address = 0x1000,
			.access = mops::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
	});

	auto zero_initial = mops::state();
	zero_initial.registers[destination_register] =
		mops::tagged_address(0x1235, 9);
	zero_initial.registers[count_register] = 0;
	zero_initial.registers[source_register] = 0xa5;
	auto zero_expected = zero_initial;
	zero_expected.nzcv = mops::option_b_forward;
	zero_expected.pc = mops::successful_program_pc(4);
	const auto zero_code = sequence(1, source_register);
	harness.check_program({
		.name = "SETG zero length unaligned sequence",
		.code = zero_code,
		.initial = zero_initial,
		.expected = zero_expected,
		.native = mops::native(true),
	});

	for (unsigned options = 0; options < 4; ++options) {
		auto initial = mops::state();
		initial.registers[destination_register] =
			mops::tagged_address(mops::data_base + 1, 13);
		initial.registers[count_register] = 17;
		initial.registers[source_register] = 0x6d;
		initial.nzcv = mops::option_b_backward;
		harness.check_one("zero-size SETG main stage",
			mops::set_tag_instruction(mops::Stage::main, options,
				destination_register, count_register, source_register),
			initial, initial, mops::native(true));
	}
}

void non_format_flags_are_ignored(engine::EngineHarness &harness)
{
	constexpr std::size_t destination_offset = 320;
	constexpr std::size_t size = 32;
	constexpr std::uint8_t logical_tag = 12;
	std::vector<std::byte> initial(mops::page_size, std::byte{0xcc});
	auto expected_memory = initial;
	mops::set_bytes(expected_memory, destination_offset, size,
		std::byte{0x6b});
	const std::array initial_tags{
		engine::AllocationTag{
			.offset = destination_offset,
			.value = 1,
		},
		engine::AllocationTag{
			.offset = destination_offset + 16,
			.value = 2,
		},
	};
	const std::array expected_tags{
		engine::AllocationTag{
			.offset = destination_offset,
			.value = logical_tag,
		},
		engine::AllocationTag{
			.offset = destination_offset + 16,
			.value = logical_tag,
		},
	};
	auto initial_cpu = mops::state();
	initial_cpu.registers[destination_register] = mops::tagged_address(
		mops::data_base + destination_offset, logical_tag);
	initial_cpu.registers[count_register] = size;
	initial_cpu.registers[source_register] = 0x6b;
	initial_cpu.nzcv = 0xf000'0000U;
	auto expected_cpu = initial_cpu;
	expected_cpu.registers[destination_register] += size;
	expected_cpu.registers[count_register] = 0;
	expected_cpu.pc = mops::successful_program_pc(2);
	const std::array code{
		engine::ProgramWord{
			.offset = 0,
			.instruction = mops::set_tag_instruction(
				mops::Stage::epilogue, 2, destination_register,
				count_register, source_register),
		},
		engine::ProgramWord{.offset = 4, .instruction = mops::system_call},
	};
	const std::array regions{engine::MemoryRegion{
		.address = mops::data_base,
		.initial = initial,
		.expected = expected_memory,
		.initial_tags = initial_tags,
		.expected_tags = expected_tags,
	}};
	harness.check_program({
		.name = "SETG epilogue ignores N/Z/V format bits",
		.code = code,
		.initial = initial_cpu,
		.expected = expected_cpu,
		.native = mops::native(true),
		.memory = regions,
	});
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	all_options_sources_pages_and_tags(harness);
	zero_maximum_and_main_stage(harness);
	non_format_flags_are_ignored(harness);
	harness.print_summary("arm64-base-mops-set-tag");
}
