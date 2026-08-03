// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>

namespace cmpbr_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;

constexpr std::size_t entry_offset = 2048;
constexpr std::uint64_t base =
	engine_tests::engine_test_code_base + entry_offset;

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

[[nodiscard]] inline arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			0x1122'3344'5566'7700ULL + index * 0x101U;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				reg * 29U + byte * 17U + 3U);
	cpu.sp = 0xcafe'0000'0000'0040ULL;
	cpu.nzcv = 0xd000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.set_mte_configuration(true, 0x8421U);
	cpu.set_mte_generator(true, 0xace1U, 7, 0xfeed'face'0123'4567ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0xa1b2'c3d4U;
	cpu.exclusive_address = 0x7654'3000ULL;
	cpu.exclusive_key = 0x1234U;
	cpu.exclusive_value = 0xa5a5'a5a5'a5a5'a5a5ULL;
	cpu.exclusive_value2 = 0x5a5a'5a5a'5a5a'5a5aULL;
	cpu.exclusive_version = 19;
	cpu.exclusive_size = 16;
	return cpu;
}

[[nodiscard]] constexpr std::string_view evidence_name(
	std::uint32_t instruction)
{
	const bool immediate = (instruction & 0x0100'0000U) != 0;
	const unsigned condition = (instruction >> 21) & 7U;
	if (immediate) {
		switch (condition) {
		case 0: return "BASE-0057 CBcc immediate BASE-0064 CBGE immediate";
		case 1: return "BASE-0057 CBcc immediate BASE-0071 CBLE immediate";
		case 2: return "BASE-0057 CBcc immediate BASE-0070 CBHS immediate";
		case 3: return "BASE-0057 CBcc immediate BASE-0074 CBLS immediate";
		default: return "BASE-0057 CBcc immediate";
		}
	}

	const unsigned narrow_control = (instruction >> 14) & 3U;
	if (narrow_control == 2U) {
		switch (condition) {
		case 0: return "BASE-0059 CBBcc BASE-0063 CBBLT";
		case 1: return "BASE-0059 CBBcc BASE-0060 CBBLE";
		case 2: return "BASE-0059 CBBcc BASE-0061 CBBLO";
		case 3: return "BASE-0059 CBBcc BASE-0062 CBBLS";
		default: return "BASE-0059 CBBcc";
		}
	}
	if (narrow_control == 3U) {
		switch (condition) {
		case 0: return "BASE-0065 CBHcc BASE-0069 CBHLT";
		case 1: return "BASE-0065 CBHcc BASE-0066 CBHLE";
		case 2: return "BASE-0065 CBHcc BASE-0067 CBHLO";
		case 3: return "BASE-0065 CBHcc BASE-0068 CBHLS";
		default: return "BASE-0065 CBHcc";
		}
	}
	switch (condition) {
	case 0: return "BASE-0058 CBcc register BASE-0076 CBLT register";
	case 1: return "BASE-0058 CBcc register BASE-0072 CBLE register";
	case 2: return "BASE-0058 CBcc register BASE-0073 CBLO register";
	case 3: return "BASE-0058 CBcc register BASE-0075 CBLS register";
	default: return "BASE-0058 CBcc register";
	}
}

class Harness final {
public:
	void check(std::uint32_t instruction, const arm64::CpuState &initial,
		bool take, std::int64_t displacement)
	{
		const auto target = static_cast<std::size_t>(
			static_cast<std::int64_t>(entry_offset) + displacement);
		if (take && target == entry_offset) {
			const std::array loop{
				engine_tests::ProgramWord{
					.offset = entry_offset,
					.instruction = instruction,
				},
			};
			auto expected = initial;
			expected.pc = base;
			engines_.check_program({
				.name = evidence_name(instruction),
				.code = loop,
				.entry_offset = entry_offset,
				.initial = initial,
				.expected = expected,
				.event = engine_tests::ExpectedEvent::instruction_budget,
				.native = native_requirements(),
			});
			return;
		}

		const auto terminal = take ? target : entry_offset + 4;
		const std::array program{
			engine_tests::ProgramWord{
				.offset = entry_offset,
				.instruction = instruction,
			},
			engine_tests::ProgramWord{
				.offset = terminal,
				.instruction = 0xd400'0001U,
			},
		};
		auto expected = initial;
		expected.pc = engine_tests::engine_test_code_base + terminal + 4;
		engines_.check_program({
			.name = evidence_name(instruction),
			.code = program,
			.entry_offset = entry_offset,
			.initial = initial,
			.expected = expected,
			.native = native_requirements(),
		});
	}

	void check_illegal(std::uint32_t instruction)
	{
		engines_.check_illegal("compare-and-branch reserved", instruction,
			state(), native_requirements());
	}

	void print_summary(std::string_view suite) const
	{
		engines_.print_summary(suite);
	}

private:
	[[nodiscard]] static engine_tests::NativeRequirements
	native_requirements()
	{
		return engine_tests::NativeRequirements::features(
			::uml::cpu::ArmFeature::cmpbr);
	}

	engine_tests::EngineHarness engines_;
};

[[nodiscard]] constexpr std::int64_t displacement(unsigned imm9)
{
	const auto shifted = static_cast<std::uint64_t>(imm9 & 0x1ffU) << 2;
	return (shifted & 0x400U) != 0 ?
		static_cast<std::int64_t>(
			shifted | ~std::uint64_t{0x7ffU}) :
		static_cast<std::int64_t>(shifted);
}

} // namespace cmpbr_test
