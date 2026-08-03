// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_control_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>

namespace uml::emulator::tests::base_control {
namespace {

namespace gadgets = arm64::gadgets;

constexpr std::array<std::int64_t, 29> immediates{
	-(std::int64_t{1} << 20),
	-(std::int64_t{1} << 19),
	-4097,
	-4096,
	-4095,
	-5,
	-4,
	-3,
	-2,
	-1,
	0,
	1,
	2,
	3,
	4,
	5,
	4095,
	4096,
	4097,
	(std::int64_t{1} << 8),
	(std::int64_t{1} << 9),
	(std::int64_t{1} << 10),
	(std::int64_t{1} << 12),
	(std::int64_t{1} << 16),
	(std::int64_t{1} << 17),
	(std::int64_t{1} << 18),
	(std::int64_t{1} << 19) - 1,
	(std::int64_t{1} << 20) - 2,
	(std::int64_t{1} << 20) - 1,
};

void semantics(Harness &harness)
{
	for (const bool page : {false, true})
		for (unsigned destination = 0; destination < 32; ++destination)
			for (unsigned index = 0; index < immediates.size(); ++index) {
				const auto immediate = immediates[index];
				const auto instruction = encode_pc_relative(
					page, immediate, destination);
				auto initial = seeded_state(
					0x1100'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(instruction) << 7) ^
					index);
				auto expected = initial;
				const auto base = page ?
					engine_test_code_base &
						~std::uint64_t{0xfff} :
					engine_test_code_base;
				const auto scale = page ? 4096 : 1;
				const auto result = static_cast<std::uint64_t>(
					static_cast<std::int64_t>(base) +
					immediate * scale);
				if (destination != 31U)
					expected.registers[destination] = result;
				char name[144];
				std::snprintf(name, sizeof(name),
					"%s rd=%u immediate=%lld",
					page ? "ADRP" : "ADR", destination,
					static_cast<long long>(immediate));
				harness.check_linear(name, instruction, initial,
					expected);
			}
}

void immediate_basis(Harness &harness)
{
	for (const bool page : {false, true})
		for (unsigned bit = 0; bit < 21; ++bit) {
			const auto immediate = bit == 20U ?
				-(std::int64_t{1} << 20) :
				std::int64_t{1} << bit;
			const auto instruction =
				encode_pc_relative(page, immediate, 13);
			auto initial = seeded_state(
				0x1110'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 5));
			auto expected = initial;
			const auto base = page ?
				engine_test_code_base & ~std::uint64_t{0xfff} :
				engine_test_code_base;
			expected.registers[13] = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(base) +
				immediate * (page ? 4096 : 1));
			char name[112];
			std::snprintf(name, sizeof(name),
				"%s immediate encoding bit=%u",
				page ? "ADRP" : "ADR", bit);
			harness.check_linear(
				name, instruction, initial, expected);
		}
}

void production_specialization()
{
	constexpr auto base = engine_test_code_base + 0xffc;
	constexpr std::uint32_t svc = terminal_system_call;
	const auto build = [&](bool page, std::int64_t immediate,
			       unsigned destination) {
		const std::array instructions{
			encode_pc_relative(page, immediate, destination), svc};
		const auto built =
			gadgets::Builder{}.build(base, instructions);
		require(built.complete,
			"PC-relative production program did not build");
		return std::move(built.program);
	};

	const auto adr_program = build(false, -12345, 7);
	const auto adrp_program = build(true, 12345, 7);
	const auto adr = adr_program.words();
	const auto adrp = adrp_program.words();
	require(adr.size() == 5U && adrp.size() == 5U,
		"PC-relative stream retained unexpected runtime operands");
	require(adr[1] == offsetof(arm64::CpuState, registers) + 7U * 8U &&
			adr[2] == static_cast<std::uint64_t>(
				static_cast<std::int64_t>(base) - 12345),
		"ADR fields were not resolved during translation");
	require(adr[0] == adrp[0],
		"ADR and ADRP did not share the constant-store gadget");
	require(adr[2] != adrp[2],
		"ADRP page calculation was not resolved during translation");

	const auto discard_program = build(false, 123, 31);
	const auto discard = discard_program.words();
	const auto nop = gadgets::Builder{}.build(
		base, std::array{std::uint32_t{0xd503'201fU}, svc});
	require(nop.complete && discard.size() == 3U &&
			discard[0] == nop.program.words()[0] &&
			discard[1] == nop.program.words()[1],
		"discarded PC-relative result was not folded to NOP");

	std::set<std::uint64_t> basis_values;
	for (unsigned bit = 0; bit < 21; ++bit) {
		const auto program =
			build(false, std::int64_t{1} << bit, 9);
		basis_values.insert(program.words()[2]);
	}
	require(basis_values.size() == 21U,
		"PC-relative immediate basis bits were lost in translation");
}

} // namespace
} // namespace uml::emulator::tests::base_control

int main()
{
	uml::emulator::tests::base_control::Harness harness;
	uml::emulator::tests::base_control::semantics(harness);
	uml::emulator::tests::base_control::immediate_basis(harness);
	uml::emulator::tests::base_control::production_specialization();
	harness.print_summary("arm64-base-control-pc-relative");
	return 0;
}
