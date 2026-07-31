// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_mops_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace mops = mops_test;
namespace engine = uml::backend::tests;

namespace {

constexpr unsigned destination_register = 3;
constexpr unsigned count_register = 4;
constexpr unsigned source_register = 5;

[[nodiscard]] constexpr std::array<engine::ProgramWord, 4>
sequence(unsigned options = 0)
{
	return {{
		{.offset = 0,
		 .instruction = mops::set_tag_instruction(mops::Stage::prologue,
			options, destination_register, count_register,
			source_register)},
		{.offset = 4,
		 .instruction = mops::set_tag_instruction(mops::Stage::main,
			options, destination_register, count_register,
			source_register)},
		{.offset = 8,
		 .instruction = mops::set_tag_instruction(mops::Stage::epilogue,
			options, destination_register, count_register,
			source_register)},
		{.offset = 12, .instruction = mops::system_call},
	}};
}

void alignment_faults(engine::EngineHarness &harness)
{
	for (const auto stage : {
	     mops::Stage::prologue, mops::Stage::epilogue}) {
		for (unsigned options = 0; options < 4; ++options) {
			for (const auto [address, count] : {
			     std::pair{mops::data_base + 1, std::uint64_t{16}},
			     std::pair{mops::data_base, std::uint64_t{17}}}) {
				auto initial = mops::state();
				initial.registers[destination_register] =
					mops::tagged_address(address, 11);
				initial.registers[count_register] = count;
				initial.registers[source_register] = 0x5a;
				initial.nzcv = mops::option_b_forward;
				harness.check({
					.name = "SETG alignment fault",
					.instruction = mops::set_tag_instruction(stage,
						options, destination_register, count_register,
						source_register),
					.initial = initial,
					.expected = initial,
					.event = engine::ExpectedEvent::memory_fault,
					.native = mops::native(true),
					.fault = {
						.address = address,
						.access =
							mops::arm64::MemoryAccess::write,
					},
					.compare_fault = true,
				});
			}
		}
	}
}

void malformed_checks_precede_alignment(engine::EngineHarness &harness)
{
	const auto instruction = mops::set_tag_instruction(
		mops::Stage::epilogue, 0, destination_register, count_register,
		source_register);
	for (const auto [count, nzcv] : {
	     std::pair{std::uint64_t{16}, 0U},
	     std::pair{std::uint64_t{0x8000'0000'0000'0000ULL},
		mops::option_b_forward}}) {
		auto initial = mops::state();
		initial.registers[destination_register] =
			mops::tagged_address(mops::data_base + 1, 9);
		initial.registers[count_register] = count;
		initial.registers[source_register] = 0xa5;
		initial.nzcv = nzcv;
		mops::check_runtime_illegal(harness,
			"malformed SETG epilogue state", instruction, initial,
			mops::native(true));
	}

	auto zero = mops::state();
	zero.registers[destination_register] =
		mops::tagged_address(mops::data_base + 1, 7);
	zero.registers[count_register] = 0;
	zero.registers[source_register] = 0xa5;
	zero.nzcv = mops::option_b_backward;
	harness.check_one("SETG zero epilogue ignores format/alignment",
		instruction, zero, zero, mops::native(true));
}

void first_page_permission_fault(engine::EngineHarness &harness)
{
	const auto denied_page = mops::data_base + 2 * mops::page_size;
	const auto destination =
		mops::tagged_address(denied_page + 32, 10);
	const std::vector<std::byte> denied_initial(mops::page_size,
		std::byte{0xcc});
	auto initial = mops::state();
	initial.registers[destination_register] = destination;
	initial.registers[count_register] = 16;
	initial.registers[source_register] = 0x5a;
	auto expected = initial;
	expected.nzcv = mops::option_b_forward;
	expected.pc = engine::engine_test_code_base + 8;
	const std::array regions{engine::MemoryRegion{
		.address = denied_page,
		.initial = denied_initial,
		.readable = true,
		.writable = false,
	}};
	const auto code = sequence(2);
	harness.check_program({
		.name = "SETG first-page permission fault",
		.code = code,
		.initial = initial,
		.expected = expected,
		.event = engine::ExpectedEvent::memory_fault,
		.native = mops::native(true),
		.fault = {
			.address = denied_page + 32,
			.access = mops::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = regions,
	});
}

void page_fault_partial_progress_and_restart(engine::EngineHarness &harness)
{
	const auto writable_page = mops::data_base + 2 * mops::page_size;
	const auto denied_page = writable_page + mops::page_size;
	const auto untagged_destination =
		writable_page + mops::page_size - 32;
	const auto destination =
		mops::tagged_address(untagged_destination, 14);
	std::vector<std::byte> writable_initial(mops::page_size,
		std::byte{0xcc});
	auto writable_expected = writable_initial;
	std::fill_n(writable_expected.end() - 32, 32, std::byte{0xa5});
	const std::vector<std::byte> denied_initial(mops::page_size,
		std::byte{0xdd});
	const std::array initial_tags{
		engine::AllocationTag{
			.offset = mops::page_size - 32,
			.value = 1,
		},
		engine::AllocationTag{
			.offset = mops::page_size - 16,
			.value = 2,
		},
	};
	const std::array expected_tags{
		engine::AllocationTag{
			.offset = mops::page_size - 32,
			.value = 14,
		},
		engine::AllocationTag{
			.offset = mops::page_size - 16,
			.value = 14,
		},
	};
	auto initial = mops::state();
	initial.registers[destination_register] = destination;
	initial.registers[count_register] = 64;
	initial.registers[source_register] = 0xa5;
	auto expected = initial;
	expected.nzcv = mops::option_b_forward;
	expected.registers[destination_register] = destination + 32;
	expected.registers[count_register] = 32;
	expected.pc = engine::engine_test_code_base + 8;
	const std::array regions{
		engine::MemoryRegion{
			.address = writable_page,
			.initial = writable_initial,
			.expected = writable_expected,
			.initial_tags = initial_tags,
			.expected_tags = expected_tags,
		},
		engine::MemoryRegion{
			.address = denied_page,
			.initial = denied_initial,
			.readable = true,
			.writable = false,
		},
	};
	const auto code = sequence(3);
	harness.check_program({
		.name = "SETG page fault partial progress",
		.code = code,
		.initial = initial,
		.expected = expected,
		.event = engine::ExpectedEvent::memory_fault,
		.native = mops::native(true),
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
	alignment_faults(harness);
	malformed_checks_precede_alignment(harness);
	first_page_permission_fault(harness);
	page_fault_partial_progress_and_restart(harness);
	harness.print_summary("arm64-base-mops-set-tag-fault");
}
