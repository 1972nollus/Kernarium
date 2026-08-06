// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <span>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

namespace {

constexpr std::uint32_t terminal_system_call = 0xd400'0001U;
constexpr std::size_t batch_size = 63;

[[nodiscard]] arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0xffff'0000'1234'0000ULL | index;
	cpu.sp = 0;
	cpu.nzcv = 0xd000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	return cpu;
}

[[nodiscard]] constexpr std::uint32_t immediate(
	unsigned offset, unsigned rn, unsigned operation)
{
	return 0xf980'0000U | (offset << 10) | (rn << 5) | operation;
}

[[nodiscard]] constexpr std::uint32_t literal(
	unsigned offset, unsigned operation)
{
	return 0xd800'0000U | (offset << 5) | operation;
}

[[nodiscard]] constexpr std::uint32_t register_offset(unsigned rm,
	unsigned option, bool scaled, unsigned rn, unsigned operation)
{
	return 0xf8a0'0800U | (rm << 16) | (option << 13) |
		(static_cast<unsigned>(scaled) << 12) | (rn << 5) | operation;
}

[[nodiscard]] constexpr std::uint32_t unscaled(
	unsigned offset, unsigned rn, unsigned operation, unsigned mode = 0)
{
	return 0xf880'0000U | (offset << 12) | (mode << 10) |
		(rn << 5) | operation;
}

void append_valid_encodings(std::vector<std::uint32_t> &stream)
{
	for (unsigned offset = 0; offset < 4096; ++offset)
		for (unsigned operation = 0; operation < 32; ++operation)
			stream.push_back(immediate(offset, 31, operation));
	for (unsigned rn = 0; rn < 32; ++rn)
		for (unsigned operation = 0; operation < 32; ++operation)
			stream.push_back(immediate(0, rn, operation));

	for (unsigned offset = 0; offset < (1U << 19); ++offset)
		stream.push_back(literal(offset, 31));
	for (unsigned operation = 0; operation < 32; ++operation)
		stream.push_back(literal(0, operation));

	for (const unsigned option : {2U, 3U, 6U, 7U})
		for (const bool scaled : {false, true})
			for (unsigned rm = 0; rm < 32; ++rm)
				for (unsigned rn = 0; rn < 32; ++rn)
					for (unsigned operation = 0; operation < 32;
					     ++operation)
						stream.push_back(register_offset(
							rm, option, scaled, rn, operation));

	for (unsigned offset = 0; offset < 512; ++offset)
		for (unsigned rn = 0; rn < 32; ++rn)
			for (unsigned operation = 0; operation < 32; ++operation)
				stream.push_back(unscaled(offset, rn, operation));
}

void check_valid_batch(engine_tests::EngineHarness &harness,
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
		.name = "prefetch-valid-variants",
		.code = std::span<const engine_tests::ProgramWord>{code},
		.initial = initial,
		.expected = expected,
		.native = engine_tests::NativeRequirements::baseline(),
	});
}

void valid_prefetches(engine_tests::EngineHarness &harness)
{
	std::vector<std::uint32_t> stream;
	stream.reserve(1'500'000);
	append_valid_encodings(stream);

	for (std::size_t first = 0; first < stream.size(); first += batch_size)
		check_valid_batch(harness, std::span<const std::uint32_t>{stream}
			.subspan(first, std::min(batch_size, stream.size() - first)));
}

void exact_evidence_representatives(engine_tests::EngineHarness &harness)
{
	const auto initial = state();
	for (const auto [name, instruction] :
	     std::array<std::pair<std::string_view, std::uint32_t>, 3>{
		     {{"BASE-0320 PRFM immediate exact", 0xf980'0000U},
		      {"BASE-0322 PRFM register exact", 0xf8a0'4800U},
		      {"BASE-0323 PRFUM exact", 0xf880'0000U}}})
		harness.check_one(name, instruction, initial, initial,
			engine_tests::NativeRequirements::baseline());
}

void reserved_register_offsets(engine_tests::EngineHarness &harness)
{
	const auto initial = state();
	for (const unsigned option : {0U, 1U, 4U, 5U})
		for (const bool scaled : {false, true})
			for (unsigned rm = 0; rm < 32; ++rm)
				for (unsigned rn = 0; rn < 32; ++rn)
					for (unsigned operation = 0; operation < 32;
					     ++operation) {
						const auto instruction = register_offset(
							rm, option, scaled, rn, operation);
						harness.check_illegal(
							"prefetch-reserved-register-offset",
							instruction, initial);
					}
	for (const unsigned mode : {1U, 3U})
		for (unsigned offset = 0; offset < 512; ++offset) {
			const auto instruction = unscaled(offset, 1, 2, mode);
			harness.check_illegal("prefetch-reserved-writeback",
				instruction, initial);
		}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	exact_evidence_representatives(harness);
	valid_prefetches(harness);
	reserved_register_offsets(harness);
	harness.print_summary("arm64-base-prefetch");
	return 0;
}
