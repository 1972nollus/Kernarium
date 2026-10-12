// SPDX-License-Identifier: GPL-2.0-or-later
/* The immediates the gadget engine packs into the operand word that already
 * carries a register: a constant store shares one word between the
 * destination's offset and a value of up to 48 bits (or of ones above bit 47).
 * Each form runs against the architectural definition for every width, shift,
 * destination and boundary value, alone and with an adjacent consumer that
 * takes the stored value from the forwarded result. */
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "require.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace g = uml::cpu::arm64::gadgets;
namespace a = uml::cpu::arm64;

extern "C" {
void uml_arm64_gadget_svc();
void uml_arm64_gadget_move_wide_constant();
void uml_arm64_gadget_move_wide_packed_zero();
void uml_arm64_gadget_move_wide_packed_ones();
}

namespace {
using uml::emulator::tests::address;
using uml::testing::require;

constexpr std::uint64_t pc = 0x4000;
constexpr std::uint32_t svc = 0xd400'0001U;

std::uint64_t next_random(std::uint64_t &seed)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return seed;
}

a::CpuState random_state(std::uint64_t &seed)
{
	a::CpuState cpu{};
	for (auto &value : cpu.registers)
		value = next_random(seed);
	cpu.pc = pc;
	cpu.sp = next_random(seed) & ~std::uint64_t{15};
	cpu.nzcv = static_cast<std::uint32_t>(next_random(seed)) & 0xf000'0000U;
	return cpu;
}

/* Runs the instructions and the SVC that ends them on both forwarding
 * settings and returns the state each leaves; they must agree. */
a::CpuState run(std::vector<std::uint32_t> code, const a::CpuState &initial)
{
	code.push_back(svc);
	auto tlb = std::make_unique<uml::memory::Tlb>();
	a::CpuState result[2]{initial, initial};
	for (unsigned forwarding = 0; forwarding < 2; ++forwarding) {
		const auto built = g::Builder{}.build(pc, code, {},
			std::pmr::get_default_resource(), {},
			{.scalar_forwarding = forwarding != 0});
		require(built.complete);
		std::size_t executed{};
		require(g::execute(built.program, result[forwarding], *tlb,
			executed) == uml::cpu::EngineEvent::system_call);
		require(executed == code.size());
	}
	require(result[0].registers == result[1].registers);
	require(result[0].nzcv == result[1].nzcv && result[0].sp == result[1].sp);
	return result[1];
}

std::uint32_t move_wide(bool wide, unsigned opc, unsigned halfword,
	unsigned immediate, unsigned destination)
{
	return (wide ? 0x8000'0000U : 0U) | 0x1280'0000U | (opc << 29) |
		(halfword << 21) | (immediate << 5) | destination;
}

/* MOVZ and MOVN from the architecture's definition: the immediate at the
 * halfword, inverted for MOVN, truncated to 32 bits for a W destination. */
std::uint64_t expected_move(bool wide, unsigned opc, unsigned halfword,
	unsigned immediate)
{
	auto value = static_cast<std::uint64_t>(immediate) << (halfword * 16);
	if (opc == 0)
		value = ~value;
	return wide ? value : value & 0xffff'ffffU;
}

void move_wide_forms()
{
	constexpr std::array immediates{0U, 1U, 0x1234U, 0x7fffU, 0x8000U,
		0xfffeU, 0xffffU};
	constexpr std::array destinations{0U, 1U, 5U, 17U, 29U, 30U, 31U};
	std::uint64_t seed = 0x2f9a'51c4'80e7'3b65ULL;
	std::size_t cases{}, packed{}, wide_only{};
	for (const bool wide : {false, true})
	for (const unsigned opc : {0U, 2U})
	for (unsigned halfword = 0; halfword < (wide ? 4U : 2U); ++halfword)
	for (const auto immediate : immediates)
	for (const auto destination : destinations) {
		const auto word = move_wide(wide, opc, halfword, immediate,
			destination);
		const auto value = expected_move(wide, opc, halfword, immediate);
		const auto initial = random_state(seed);

		auto expected = initial;
		if (destination != 31U)
			expected.registers[destination] = value;
		const auto alone = run({word}, initial);
		require(alone.registers == expected.registers);
		require(alone.nzcv == expected.nzcv && alone.sp == expected.sp);

		/* The store's value reaches the next instruction either through
		 * the forwarded result or through the register. */
		if (destination != 31U) {
			const unsigned result = destination == 20U ? 21U : 20U;
			auto consumed = expected;
			consumed.registers[result] = (wide ? value :
				value & 0xffff'ffffU) + 1U;
			if (!wide)
				consumed.registers[result] &= 0xffff'ffffU;
			const auto sum = 0x1100'0400U | (wide ? 0x8000'0000U : 0U) |
				(destination << 5) | result;
			const auto after = run({word, sum}, initial);
			require(after.registers == consumed.registers);
		}

		/* The stream: one operand word when the value fits 48 bits or is
		 * ones above them, the offset and the value in two otherwise. */
		const auto built = g::Builder{}.build(pc,
			std::array{word, svc});
		const auto words = built.program.words();
		if (destination == 31U)
			continue;
		const auto offset = std::uint64_t{destination} * 8U;
		constexpr std::uint64_t low_48 = (std::uint64_t{1} << 48) - 1;
		if ((value & ~low_48) == 0) {
			require(words[0] == address(
				uml_arm64_gadget_move_wide_packed_zero));
			require(words[1] == (offset << 48 | value));
			require(words[2] == address(uml_arm64_gadget_svc));
			++packed;
		} else if ((value & ~low_48) == ~low_48) {
			require(words[0] == address(
				uml_arm64_gadget_move_wide_packed_ones));
			require(words[1] == (offset << 48 | (value & low_48)));
			require(words[2] == address(uml_arm64_gadget_svc));
			++packed;
		} else {
			require(words[0] == address(
				uml_arm64_gadget_move_wide_constant));
			require(words[1] == offset && words[2] == value);
			require(words[3] == address(uml_arm64_gadget_svc));
			++wide_only;
		}
		++cases;
	}
	require(packed > 0 && wide_only > 0);
	std::printf("move wide: %zu cases, %zu in one word, %zu in two\n",
		cases, packed, wide_only);
}

} // namespace

int main()
{
	move_wide_forms();
	std::puts("gadget packed immediate tests passed");
}
