// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <vector>

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint32_t terminal_system_call = 0xd400'0001U;
constexpr std::size_t batch_size = 63;

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

[[nodiscard]] constexpr std::uint32_t rprfm(unsigned operation,
	unsigned metadata, unsigned base)
{
	/* operation = option<2>:option<0>:S:Rt<2:0>; option<1> and
	 * Rt<4:3> are the fixed RPRFM allocation bits. */
	const unsigned option = (((operation >> 5) & 1U) << 2) | 2U |
		((operation >> 4) & 1U);
	const unsigned scale = (operation >> 3) & 1U;
	const unsigned rt = 24U | (operation & 7U);
	return 0xf8a0'0800U | ((metadata & 31U) << 16) |
		(option << 13) | (scale << 12) | ((base & 31U) << 5) | rt;
}

[[nodiscard]] arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0x9000'0000'1000'0000ULL |
			(static_cast<std::uint64_t>(index) << 32) | index;
	cpu.sp = 0xffff'ffff'ffff'fff0ULL;
	cpu.nzcv = 0xb000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	return cpu;
}

void encoding_examples()
{
	require(rprfm(0, 3, 2) == 0xf8a3'4858U); // PLDKEEP
	require(rprfm(1, 3, 2) == 0xf8a3'4859U); // PSTKEEP
	require(rprfm(4, 3, 2) == 0xf8a3'485cU); // PLDSTRM
	require(rprfm(5, 3, 2) == 0xf8a3'485dU); // PSTSTRM
	require(rprfm(63, 30, 31) == 0xf8be'fbffU);
}

void check_batch(engine_tests::EngineHarness &harness,
	std::span<const std::uint32_t> instructions)
{
	std::vector<engine_tests::ProgramWord> code;
	code.reserve(instructions.size() + 1);
	for (std::size_t index = 0; index < instructions.size(); ++index)
		code.push_back({
			.offset = index * sizeof(std::uint32_t),
			.instruction = instructions[index],
		});
	code.push_back({
		.offset = instructions.size() * sizeof(std::uint32_t),
		.instruction = terminal_system_call,
	});
	const auto initial = state();
	auto expected = initial;
	expected.pc = engine_tests::engine_test_code_base +
		code.size() * sizeof(std::uint32_t);
	harness.check_program({
		.name = "range-prefetch-variants",
		.code = std::span<const engine_tests::ProgramWord>{code},
		.initial = initial,
		.expected = expected,
		.native = engine_tests::NativeRequirements::features(
			uml::backend::ArmFeature::rprfm),
	});
}

void exhaustive_operations_registers_and_no_fault_behavior(
	engine_tests::EngineHarness &harness)
{
	std::vector<std::uint32_t> batch;
	batch.reserve(batch_size);
	for (unsigned operation = 0; operation < 64; ++operation) {
		for (unsigned metadata = 0; metadata < 32; ++metadata) {
			for (unsigned base = 0; base < 32; ++base) {
				batch.push_back(rprfm(operation, metadata, base));
				if (batch.size() == batch_size) {
					check_batch(harness, batch);
					batch.clear();
				}
			}
		}
	}
	if (!batch.empty())
		check_batch(harness, batch);
}

void production_is_a_direct_nop_gadget()
{
	const auto nop = gadgets::Builder{}.build(
		engine_tests::engine_test_code_base,
		std::array{0xd503'201fU});
	for (unsigned operation = 0; operation < 64; ++operation) {
		const auto built = gadgets::Builder{}.build(
			engine_tests::engine_test_code_base,
			std::array{rprfm(operation, 30, 31)});
		require(built.complete && built.program.words().front() ==
			nop.program.words().front());
	}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	encoding_examples();
	exhaustive_operations_registers_and_no_fault_behavior(harness);
	production_is_a_direct_nop_gadget();
	harness.print_summary("arm64-base-range-prefetch");
}
