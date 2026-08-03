// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_rmw_engine_test_support.hpp"

#include <array>

namespace atomic = arm64_atomic_rmw_engine_test;
namespace atomic_base = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

namespace {

void chromium_register_overlap(engine::EngineHarness &harness)
{
	atomic_base::Page initial_page{};
	atomic_base::put(initial_page, atomic_base::data_offset, 1, 4);
	auto expected_page = initial_page;
	atomic_base::put(expected_page, atomic_base::data_offset, 0, 4);
	const auto memory = atomic_base::region(initial_page, expected_page);
	const std::array code{
		engine::ProgramWord{0, 0xb8a0'8020U},
		engine::ProgramWord{4, 0xd65f'03c0U},
		engine::ProgramWord{8, atomic_base::terminal_system_call},
	};
	auto initial = atomic_base::state(109);
	initial.registers[0] = 0;
	initial.registers[1] = atomic_base::data_address;
	initial.registers[30] = engine::engine_test_code_base + 8;
	auto expected = initial;
	expected.registers[0] = 1;
	expected.pc = engine::engine_test_code_base + 12;
	harness.check_program({
		.name = "32-bit acquire swap same source and destination with return",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = atomic_base::lse(),
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

} // namespace

int main()
{
	constexpr std::array operations{8U};
	engine::EngineHarness harness;
	for (const auto operation : operations)
		atomic::run_operation(harness, operation);
	chromium_register_overlap(harness);
	atomic::invalid_encodings(harness);
	harness.print_summary("arm64-base-atomic-swap");
}
