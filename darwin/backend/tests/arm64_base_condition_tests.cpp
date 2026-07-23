// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

class NullMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t, std::span<std::byte>) override
	{
		return false;
	}
	[[nodiscard]] bool write(
		std::uint64_t, std::span<const std::byte>) override
	{
		return false;
	}
};

void require(bool condition,
	     const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

[[nodiscard]] constexpr bool condition_holds(
	std::uint32_t nzcv, unsigned condition)
{
	const bool negative = (nzcv & 0x8000'0000U) != 0;
	const bool zero = (nzcv & 0x4000'0000U) != 0;
	const bool carry = (nzcv & 0x2000'0000U) != 0;
	const bool overflow = (nzcv & 0x1000'0000U) != 0;
	bool result = false;
	switch (condition >> 1) {
	case 0: result = zero; break;
	case 1: result = carry; break;
	case 2: result = negative; break;
	case 3: result = overflow; break;
	case 4: result = carry && !zero; break;
	case 5: result = negative == overflow; break;
	case 6: result = !zero && negative == overflow; break;
	case 7: result = true; break;
	default: break;
	}
	return (condition & 1U) != 0 && condition < 14 ? !result : result;
}

constexpr std::uint64_t base = 0x9000;

class Harness final {
public:
	Harness() : tlb_(std::make_unique<gadgets::Tlb>()) {}

	void check(std::uint32_t instruction, const arm64::CpuState &initial,
		   std::uint32_t expected_nzcv, unsigned destination = 31,
		   std::uint64_t expected_value = 0)
	{
		auto interpreted = initial;
		interpreted.pc = base;
		require(arm64::Dispatcher{}.execute(
			interpreted, memory_, instruction).reason ==
			arm64::StopReason::continued);
		require(interpreted.nzcv == expected_nzcv);
		if (destination != 31)
			require(interpreted.registers[destination] == expected_value);

		const std::array stream{instruction, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(base, stream);
		require(built.complete);
		auto gadget = initial;
		gadget.pc = base;
		tlb_->prepare(0, darwin_uml_static_vm_changes(0));
		require(gadgets::execute(built.program, gadget, *tlb_) ==
			gadgets::Event::system_call);
		require(gadget.nzcv == expected_nzcv);
		if (destination != 31)
			require(gadget.registers[destination] == expected_value);
	}

	void check_reserved(std::uint32_t instruction)
	{
		arm64::CpuState cpu{};
		cpu.pc = base;
		require(arm64::Dispatcher{}.execute(cpu, memory_, instruction).reason ==
			arm64::StopReason::unsupported_instruction);
		const std::array stream{instruction, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(base, stream);
		require(built.complete);
		tlb_->prepare(0, darwin_uml_static_vm_changes(0));
		require(gadgets::execute(built.program, cpu, *tlb_) ==
			gadgets::Event::fatal);
	}

private:
	NullMemory memory_;
	std::unique_ptr<gadgets::Tlb> tlb_;
};

[[nodiscard]] constexpr std::uint32_t select_instruction(
	bool wide, unsigned operation, unsigned condition, unsigned rn = 1,
	unsigned rm = 2, unsigned rd = 7)
{
	return 0x1a80'0000U | (wide ? 0x8000'0000U : 0) |
		((operation & 2U) << 29) | (rm << 16) | (condition << 12) |
		((operation & 1U) << 10) | (rn << 5) | rd;
}

[[nodiscard]] constexpr std::uint32_t compare_instruction(
	bool wide, bool subtract, bool immediate, unsigned condition,
	unsigned operand, unsigned alternative, unsigned rn = 1)
{
	return 0x3a40'0000U | (wide ? 0x8000'0000U : 0) |
		(subtract ? 0x4000'0000U : 0) | ((operand & 31U) << 16) |
		(condition << 12) | (immediate ? 0x800U : 0) | (rn << 5) |
		(alternative & 15U);
}

void conditional_selects(Harness &harness)
{
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 4; ++operation)
			for (unsigned condition = 0; condition < 16; ++condition)
				for (unsigned flags = 0; flags < 16; ++flags) {
					const auto mask = wide ? ~std::uint64_t{0} :
						std::uint64_t{0xffff'ffffU};
					arm64::CpuState initial{};
					initial.nzcv = flags << 28;
					initial.registers[1] = 0x0123'4567'89ab'cdefULL;
					initial.registers[2] = 0xfedc'ba98'7654'3210ULL;
					std::uint64_t expected;
					if (condition_holds(initial.nzcv, condition)) {
						expected = initial.registers[1];
					} else if (operation == 0) {
						expected = initial.registers[2];
					} else if (operation == 1) {
						expected = initial.registers[2] + 1;
					} else if (operation == 2) {
						expected = ~initial.registers[2];
					} else {
						expected = -initial.registers[2];
					}
					harness.check(select_instruction(wide, operation,
						condition), initial, initial.nzcv, 7,
						expected & mask);
				}
}

void conditional_select_aliases(Harness &harness)
{
	/* CINC/CINV/CNEG use equal nonzero sources; CSET/CSETM use W/XZR. */
	for (const bool wide : {false, true})
		for (unsigned condition = 0; condition < 14; ++condition)
			for (unsigned flags = 0; flags < 16; ++flags) {
				const auto mask = wide ? ~std::uint64_t{0} :
					std::uint64_t{0xffff'ffffU};
				arm64::CpuState initial{};
				initial.nzcv = flags << 28;
				initial.registers[1] = 0x8000'0000'0000'0001ULL;
				const bool holds = condition_holds(initial.nzcv, condition);
				for (unsigned operation = 1; operation < 4; ++operation) {
					std::uint64_t transformed;
					if (operation == 1)
						transformed = initial.registers[1] + 1;
					else if (operation == 2)
						transformed = ~initial.registers[1];
					else
						transformed = -initial.registers[1];
					harness.check(select_instruction(wide, operation,
						condition, 1, 1), initial, initial.nzcv, 7,
						(holds ? initial.registers[1] : transformed) & mask);
				}
				harness.check(select_instruction(wide, 1, condition, 31, 31),
					initial, initial.nzcv, 7, holds ? 0 : 1);
				harness.check(select_instruction(wide, 2, condition, 31, 31),
					initial, initial.nzcv, 7, holds ? 0 : mask);
			}

	arm64::CpuState initial{};
	initial.nzcv = 0x4000'0000U;
	initial.registers[7] = 0x1234;
	harness.check(select_instruction(true, 0, 0, 31, 2), initial,
		initial.nzcv, 7, 0);
	harness.check(select_instruction(true, 0, 0, 1, 2, 31), initial,
		initial.nzcv);
}

[[nodiscard]] std::uint32_t arithmetic_flags(
	std::uint64_t left, std::uint64_t right, bool wide, bool subtract)
{
	const auto mask = wide ? ~std::uint64_t{0} :
		std::uint64_t{0xffff'ffffU};
	const auto sign = std::uint64_t{1} << (wide ? 63 : 31);
	left &= mask;
	right &= mask;
	const auto result = (subtract ? left - right : left + right) & mask;
	const bool carry = subtract ? left >= right :
		(wide ? static_cast<unsigned __int128>(left) + right > mask :
		 left + right > mask);
	const bool overflow = subtract ?
		((left ^ right) & (left ^ result) & sign) != 0 :
		((~(left ^ right)) & (left ^ result) & sign) != 0;
	return (result & sign ? 0x8000'0000U : 0) |
		(result == 0 ? 0x4000'0000U : 0) |
		(carry ? 0x2000'0000U : 0) |
		(overflow ? 0x1000'0000U : 0);
}

void conditional_compares(Harness &harness)
{
	struct Pair { std::uint64_t left; std::uint64_t right; };
	const std::array pairs{
		Pair{0, 0}, Pair{0, 1}, Pair{1, 1},
		Pair{0x7fff'ffff'ffff'ffffULL, 1},
		Pair{0x8000'0000'0000'0000ULL, ~std::uint64_t{0}},
		Pair{~std::uint64_t{0}, 31}};
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool immediate : {false, true})
				for (unsigned condition = 0; condition < 16; ++condition)
					for (unsigned flags = 0; flags < 16; ++flags)
						for (const auto [left, right] : pairs) {
							const unsigned alternative =
								(flags * 5U + condition) & 15U;
							const auto operand = immediate ?
								right & 31U : right;
							arm64::CpuState initial{};
							initial.nzcv = flags << 28;
							initial.registers[1] = left;
							initial.registers[2] = right;
							const auto expected = condition_holds(
								initial.nzcv, condition) ?
								arithmetic_flags(left, operand, wide,
									subtract) : alternative << 28;
							harness.check(compare_instruction(wide,
								subtract, immediate, condition,
								immediate ? static_cast<unsigned>(right) : 2,
								alternative), initial, expected);
						}

	/* Architectural zero-register sources for register forms. */
	arm64::CpuState initial{};
	initial.nzcv = 0x4000'0000U;
	initial.registers[2] = 1;
	harness.check(compare_instruction(true, true, false, 0, 2, 0, 31),
		initial, arithmetic_flags(0, 1, true, true));
	harness.check(compare_instruction(true, false, false, 0, 31, 0),
		initial, arithmetic_flags(0, 0, true, false));
}

} // namespace

int main()
{
	Harness harness;
	conditional_selects(harness);
	conditional_select_aliases(harness);
	conditional_compares(harness);
	harness.check_reserved(0x1a80'0800U); // conditional-select bit 11 is 0
	harness.check_reserved(0x3a40'0400U); // conditional-compare o2 is 0
	harness.check_reserved(0x3a40'0010U); // conditional-compare o3 is 0
	return 0;
}
