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

constexpr std::uint64_t base = 0xa000;

class Harness final {
public:
	Harness() : tlb_(std::make_unique<gadgets::Tlb>()) {}

	void check(std::uint32_t instruction, const arm64::CpuState &initial,
		   const arm64::CpuState &expected)
	{
		auto interpreted = initial;
		interpreted.pc = base;
		require(arm64::Dispatcher{}.execute(
			interpreted, memory_, instruction).reason ==
			arm64::StopReason::continued);
		require(interpreted.registers == expected.registers);
		require(interpreted.sp == expected.sp);
		require(interpreted.nzcv == expected.nzcv);

		const std::array stream{instruction, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(base, stream);
		require(built.complete);
		auto gadget = initial;
		gadget.pc = base;
		tlb_->prepare(0, darwin_uml_static_vm_changes(0));
		require(gadgets::execute(built.program, gadget, *tlb_) ==
			gadgets::Event::system_call);
		require(gadget.registers == expected.registers);
		require(gadget.sp == expected.sp);
		require(gadget.nzcv == expected.nzcv);
	}

	void check_undefined(std::uint32_t instruction)
	{
		arm64::CpuState cpu{};
		cpu.pc = base;
		require(arm64::Dispatcher{}.execute(cpu, memory_, instruction).reason ==
			arm64::StopReason::unsupported_instruction);
		const std::array stream{instruction, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(base, stream);
		if (built.complete) {
			tlb_->prepare(0, darwin_uml_static_vm_changes(0));
			require(gadgets::execute(built.program, cpu, *tlb_) ==
				gadgets::Event::fatal);
		}
	}

private:
	NullMemory memory_;
	std::unique_ptr<gadgets::Tlb> tlb_;
};

[[nodiscard]] constexpr std::uint64_t mask_for(bool wide)
{
	return wide ? ~std::uint64_t{0} : std::uint64_t{0xffff'ffffU};
}

[[nodiscard]] std::uint32_t arithmetic_flags(
	std::uint64_t left, std::uint64_t right, bool wide, bool subtract)
{
	const auto mask = mask_for(wide);
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

void set_expected(arm64::CpuState &expected, bool set_flags, bool wide,
		  bool subtract, std::uint64_t left, std::uint64_t right,
		  std::uint64_t result, unsigned rd, bool sp_destination)
{
	if (set_flags)
		expected.nzcv = arithmetic_flags(left, right, wide, subtract);
	if (rd != 31)
		expected.registers[rd] = result & mask_for(wide);
	else if (sp_destination && !set_flags)
		expected.sp = result & mask_for(wide);
}

[[nodiscard]] constexpr std::uint32_t immediate_instruction(
	bool wide, bool subtract, bool set_flags, bool shift12, unsigned immediate,
	unsigned rn = 1, unsigned rd = 7)
{
	return 0x1100'0000U | (wide ? 0x8000'0000U : 0) |
		(subtract ? 0x4000'0000U : 0) |
		(set_flags ? 0x2000'0000U : 0) |
		(shift12 ? 0x0040'0000U : 0) | ((immediate & 0xfffU) << 10) |
		(rn << 5) | rd;
}

[[nodiscard]] constexpr std::uint32_t shifted_instruction(
	bool wide, bool subtract, bool set_flags, unsigned shift_type,
	unsigned amount, unsigned rn = 1, unsigned rm = 2, unsigned rd = 7)
{
	return 0x0b00'0000U | (wide ? 0x8000'0000U : 0) |
		(subtract ? 0x4000'0000U : 0) |
		(set_flags ? 0x2000'0000U : 0) | (shift_type << 22) |
		(rm << 16) | (amount << 10) | (rn << 5) | rd;
}

[[nodiscard]] constexpr std::uint32_t extended_instruction(
	bool wide, bool subtract, bool set_flags, unsigned option, unsigned amount,
	unsigned rn = 1, unsigned rm = 2, unsigned rd = 7)
{
	return 0x0b20'0000U | (wide ? 0x8000'0000U : 0) |
		(subtract ? 0x4000'0000U : 0) |
		(set_flags ? 0x2000'0000U : 0) | (rm << 16) |
		(option << 13) | (amount << 10) | (rn << 5) | rd;
}

void immediate_forms(Harness &harness)
{
	const std::array values{
		std::uint64_t{0}, std::uint64_t{1},
		std::uint64_t{0x7fff'ffffU}, std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		~std::uint64_t{0}};
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (const bool shift12 : {false, true})
					for (const unsigned immediate : {0U, 1U, 0x7ffU, 0xfffU})
						for (const auto left : values) {
							const auto right = std::uint64_t{immediate} <<
								(shift12 ? 12 : 0);
							const auto result = subtract ? left - right :
								left + right;
							arm64::CpuState initial{};
							initial.nzcv = 0xf000'0000U;
							initial.registers[1] = left;
							auto expected = initial;
							set_expected(expected, set_flags, wide, subtract,
								left, right, result, 7, false);
							harness.check(immediate_instruction(wide,
								subtract, set_flags, shift12, immediate),
								initial, expected);
						}

	/* SP input/output, MOV-to/from-SP, and CMN/CMP aliases. */
	for (const bool wide : {false, true}) {
		arm64::CpuState initial{};
		initial.nzcv = 0x1000'0000U;
		initial.sp = 0xffff'ffff'8000'0001ULL;
		initial.registers[1] = 0x1234;
		auto expected = initial;
		set_expected(expected, false, wide, false, initial.sp, 0,
			initial.sp, 7, false);
		harness.check(immediate_instruction(wide, false, false, false, 0,
			31, 7), initial, expected); // MOV from SP
		expected = initial;
		set_expected(expected, false, wide, false, initial.registers[1], 0,
			initial.registers[1], 31, true);
		harness.check(immediate_instruction(wide, false, false, false, 0,
			1, 31), initial, expected); // MOV to SP
		for (const bool subtract : {false, true}) {
			expected = initial;
			set_expected(expected, true, wide, subtract, initial.sp, 1,
				subtract ? initial.sp - 1 : initial.sp + 1, 31, false);
			harness.check(immediate_instruction(wide, subtract, true,
				false, 1, 31, 31), initial, expected);
		}
	}
}

[[nodiscard]] std::uint64_t shifted_value(
	std::uint64_t value, bool wide, unsigned type, unsigned amount)
{
	value &= mask_for(wide);
	if (type == 0)
		return (value << amount) & mask_for(wide);
	if (type == 1)
		return value >> amount;
	if (wide)
		return static_cast<std::uint64_t>(
			static_cast<std::int64_t>(value) >> amount);
	return static_cast<std::uint32_t>(
		static_cast<std::int32_t>(value) >> amount);
}

void shifted_forms(Harness &harness)
{
	const std::array pairs{
		std::array{std::uint64_t{0}, std::uint64_t{0}},
		std::array{std::uint64_t{1}, std::uint64_t{1}},
		std::array{std::uint64_t{0x8000'0000U}, std::uint64_t{0xffff'ffffU}},
		std::array{std::uint64_t{0x7fff'ffff'ffff'ffffULL},
			std::uint64_t{0x8000'0000'0000'0000ULL}},
		std::array{~std::uint64_t{0}, std::uint64_t{0x0123'4567'89ab'cdefULL}}};
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (unsigned type = 0; type < 3; ++type)
					for (const unsigned amount : {0U, 1U,
						wide ? 31U : 15U, wide ? 63U : 31U})
						for (const auto &pair : pairs) {
							const auto right = shifted_value(pair[1], wide,
								type, amount);
							const auto result = subtract ? pair[0] - right :
								pair[0] + right;
							arm64::CpuState initial{};
							initial.nzcv = 0x6000'0000U;
							initial.registers[1] = pair[0];
							initial.registers[2] = pair[1];
							auto expected = initial;
							set_expected(expected, set_flags, wide, subtract,
								pair[0], right, result, 7, false);
							harness.check(shifted_instruction(wide,
								subtract, set_flags, type, amount),
								initial, expected);
						}

	/* NEG/NEGS and shifted CMN/CMP aliases, including XZR/WZR. */
	for (const bool wide : {false, true})
		for (const bool set_flags : {false, true}) {
			arm64::CpuState initial{};
			initial.nzcv = 0x9000'0000U;
			initial.registers[2] = 0x8000'0000'0000'0001ULL;
			const auto right = shifted_value(initial.registers[2], wide, 0, 0);
			auto expected = initial;
			set_expected(expected, set_flags, wide, true, 0, right,
				-right, 7, false);
			harness.check(shifted_instruction(wide, true, set_flags, 0, 0,
				31, 2, 7), initial, expected);
			for (const bool subtract : {false, true}) {
				expected = initial;
				set_expected(expected, true, wide, subtract, 0, right,
					subtract ? -right : right, 31, false);
				harness.check(shifted_instruction(wide, subtract, true, 0,
					0, 31, 2, 31), initial, expected);
			}
		}

	harness.check_undefined(shifted_instruction(true, false, false, 3, 0));
	harness.check_undefined(shifted_instruction(false, false, false, 0, 32));
}

[[nodiscard]] std::uint64_t extended_value(
	std::uint64_t value, bool wide, unsigned option, unsigned amount)
{
	const unsigned bits = 8U << (option & 3U);
	const auto source_mask = bits == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << bits) - 1;
	value &= source_mask;
	if ((option & 4U) != 0 &&
	    (value & (std::uint64_t{1} << (bits - 1))) != 0)
		value |= ~source_mask;
	return (value << amount) & mask_for(wide);
}

void extended_forms(Harness &harness)
{
	const std::array values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x80},
		std::uint64_t{0x8000}, std::uint64_t{0x8000'0000U},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		~std::uint64_t{0}};
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (unsigned option = 0; option < 8; ++option)
					for (unsigned amount = 0; amount <= 4; ++amount)
						for (const auto value : values) {
							const auto left = 0x7fff'ffff'ffff'fff0ULL;
							const auto right = extended_value(value, wide,
								option, amount);
							const auto result = subtract ? left - right :
								left + right;
							arm64::CpuState initial{};
							initial.nzcv = 0x3000'0000U;
							initial.registers[1] = left;
							initial.registers[2] = value;
							auto expected = initial;
							set_expected(expected, set_flags, wide, subtract,
								left, right, result, 7, false);
							harness.check(extended_instruction(wide,
								subtract, set_flags, option, amount),
								initial, expected);
						}

	/* Extended forms use SP for Rn and for non-flag-setting Rd==31. */
	for (const bool wide : {false, true}) {
		arm64::CpuState initial{};
		initial.nzcv = 0xa000'0000U;
		initial.sp = 0xffff'ffff'ffff'fff0ULL;
		initial.registers[2] = 0xff;
		const auto right = extended_value(initial.registers[2], wide, 4, 1);
		auto expected = initial;
		set_expected(expected, false, wide, false, initial.sp, right,
			initial.sp + right, 31, true);
		harness.check(extended_instruction(wide, false, false, 4, 1,
			31, 2, 31), initial, expected);
		for (const bool subtract : {false, true}) {
			expected = initial;
			set_expected(expected, true, wide, subtract, initial.sp, right,
				subtract ? initial.sp - right : initial.sp + right,
				31, false);
			harness.check(extended_instruction(wide, subtract, true, 4, 1,
				31, 2, 31), initial, expected);
		}
	}
	harness.check_undefined(extended_instruction(true, false, false, 0, 5));
}

} // namespace

int main()
{
	Harness harness;
	immediate_forms(harness);
	shifted_forms(harness);
	extended_forms(harness);
	return 0;
}
