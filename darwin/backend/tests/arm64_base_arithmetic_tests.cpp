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

struct AddWithCarryResult final {
	std::uint64_t value;
	std::uint32_t nzcv;
};

[[nodiscard]] AddWithCarryResult reference_add_with_carry(
	std::uint64_t left, std::uint64_t right, bool carry, bool subtract,
	bool wide)
{
	const unsigned width = wide ? 64 : 32;
	const auto mask = wide ? ~std::uint64_t{0} :
		std::uint64_t{0xffff'ffffU};
	left &= mask;
	right &= mask;
	const auto second = subtract ? mask ^ right : right;
	const unsigned __int128 unsigned_sum =
		static_cast<unsigned __int128>(left) + second + carry;
	const auto result = static_cast<std::uint64_t>(unsigned_sum) & mask;
	const auto signed_value = [width](std::uint64_t value) -> __int128 {
		const auto sign = std::uint64_t{1} << (width - 1);
		if ((value & sign) == 0)
			return value;
		return static_cast<__int128>(value) -
			(static_cast<__int128>(1) << width);
	};
	const __int128 signed_sum = signed_value(left) + signed_value(second) +
		static_cast<unsigned>(carry);
	const __int128 signed_minimum =
		-(static_cast<__int128>(1) << (width - 1));
	const __int128 signed_maximum =
		(static_cast<__int128>(1) << (width - 1)) - 1;
	const bool overflow = signed_sum < signed_minimum ||
		signed_sum > signed_maximum;
	const bool negative = (result & (std::uint64_t{1} << (width - 1))) != 0;
	const bool zero = result == 0;
	const bool carry_out = (unsigned_sum >> width) != 0;
	return {result,
		(negative ? 0x8000'0000U : 0) |
		(zero ? 0x4000'0000U : 0) |
		(carry_out ? 0x2000'0000U : 0) |
		(overflow ? 0x1000'0000U : 0)};
}

[[nodiscard]] std::uint32_t encode_carry(
	bool wide, bool subtract, bool set_flags, unsigned d, unsigned n, unsigned m)
{
	return 0x1a00'0000U | (wide ? 0x8000'0000U : 0) |
		(subtract ? 0x4000'0000U : 0) |
		(set_flags ? 0x2000'0000U : 0) | (m << 16) | (n << 5) | d;
}

void check_case(std::uint32_t instruction, std::uint64_t left,
		std::uint64_t right, std::uint32_t initial_nzcv,
		const AddWithCarryResult &reference, bool set_flags,
		gadgets::Tlb &tlb)
{
	constexpr std::uint64_t base = 0x4000;
	arm64::CpuState initial{};
	initial.pc = base;
	initial.nzcv = initial_nzcv;
	initial.registers[1] = left;
	initial.registers[2] = right;
	initial.registers[7] = 0xfeed'face'dead'beefULL;
	const auto d = instruction & 31U;
	const bool wide = (instruction >> 31) != 0;
	const auto expected_value = wide ? reference.value :
		reference.value & 0xffff'ffffU;
	const auto expected_nzcv = set_flags ? reference.nzcv : initial_nzcv;

	NullMemory memory;
	auto interpreted = initial;
	require(arm64::Dispatcher{}.execute(
		interpreted, memory, instruction).reason ==
		arm64::StopReason::continued);
	require(interpreted.pc == base + sizeof(std::uint32_t));
	require(interpreted.nzcv == expected_nzcv);
	if (d != 31)
		require(interpreted.registers[d] == expected_value);
	else
		require(interpreted.registers[7] == initial.registers[7]);

	const std::array stream{instruction, 0xd400'0001U};
	const auto built = gadgets::Builder{}.build(base, stream);
	require(built.complete);
	auto gadget = initial;
	tlb.prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, tlb) ==
		gadgets::Event::system_call);
	require(gadget.nzcv == expected_nzcv);
	if (d != 31)
		require(gadget.registers[d] == expected_value);
	else
		require(gadget.registers[7] == initial.registers[7]);
	for (std::size_t index = 0; index < initial.registers.size(); ++index) {
		if (index != d)
			require(gadget.registers[index] == initial.registers[index]);
	}
}

void carry_arithmetic()
{
	const std::array values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U}, std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		~std::uint64_t{0}};
	auto tlb = std::make_unique<gadgets::Tlb>();
	for (const bool wide : {false, true}) {
		const auto width_mask = wide ? ~std::uint64_t{0} :
			std::uint64_t{0xffff'ffffU};
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (const bool carry : {false, true})
					for (const auto left : values)
						for (const auto right : values) {
							const auto initial_nzcv =
								0x9000'0000U |
								(carry ? 0x2000'0000U : 0);
							const auto reference = reference_add_with_carry(
								left & width_mask, right & width_mask,
								carry, subtract, wide);
							check_case(encode_carry(wide, subtract,
								set_flags, 7, 1, 2), left, right,
								initial_nzcv, reference, set_flags, *tlb);
						}
	}

	/* Rn=ZR is the NGC/NGCS alias. Rm=ZR, Rd=ZR, and both carry
	 * states additionally verify architectural zero-register behavior. */
	for (const bool wide : {false, true})
		for (const bool set_flags : {false, true})
			for (const bool carry : {false, true})
				for (const unsigned m : {2U, 31U})
					for (const unsigned d : {7U, 31U}) {
						const auto right = m == 31 ? 0 :
							0x8000'0000'0000'0001ULL;
						const auto reference = reference_add_with_carry(
							0, right, carry, true, wide);
						const auto initial_nzcv = 0x9000'0000U |
							(carry ? 0x2000'0000U : 0);
						check_case(encode_carry(wide, true, set_flags,
							d, 31, m), 0, right, initial_nzcv,
							reference, set_flags, *tlb);
					}
}

[[nodiscard]] std::uint64_t reference_divide(
	std::uint64_t dividend, std::uint64_t divisor, bool signed_divide,
	bool wide)
{
	const auto mask = wide ? ~std::uint64_t{0} :
		std::uint64_t{0xffff'ffffU};
	dividend &= mask;
	divisor &= mask;
	if (divisor == 0)
		return 0;
	if (!signed_divide)
		return dividend / divisor;
	if (wide) {
		if (dividend == (std::uint64_t{1} << 63) && divisor == mask)
			return dividend;
		return static_cast<std::uint64_t>(
			static_cast<std::int64_t>(dividend) /
			static_cast<std::int64_t>(divisor));
	}
	if (dividend == (std::uint32_t{1} << 31) &&
	    divisor == 0xffff'ffffU)
		return dividend;
	return static_cast<std::uint32_t>(
		static_cast<std::int32_t>(dividend) /
		static_cast<std::int32_t>(divisor));
}

void division()
{
	constexpr std::uint64_t base = 0x5000;
	const std::array values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2},
		std::uint64_t{0x7fff'ffffU}, std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		~std::uint64_t{0}};
	auto tlb = std::make_unique<gadgets::Tlb>();
	NullMemory memory;
	for (const bool wide : {false, true})
		for (const bool signed_divide : {false, true})
			for (const auto dividend : values)
				for (const auto divisor : values) {
					const std::uint32_t instruction =
						0x1ac0'0800U |
						(wide ? 0x8000'0000U : 0) |
						(signed_divide ? 0x400U : 0) |
						(2U << 16) | (1U << 5) | 7U;
					const auto expected = reference_divide(
						dividend, divisor, signed_divide, wide);
					arm64::CpuState initial{};
					initial.pc = base;
					initial.nzcv = 0xf000'0000U;
					initial.registers[1] = dividend;
					initial.registers[2] = divisor;
					auto interpreted = initial;
					require(arm64::Dispatcher{}.execute(
						interpreted, memory, instruction).reason ==
						arm64::StopReason::continued);
					require(interpreted.registers[7] == expected);
					require(interpreted.nzcv == initial.nzcv);
					const std::array stream{instruction, 0xd400'0001U};
					const auto built = gadgets::Builder{}.build(base, stream);
					require(built.complete);
					auto gadget = initial;
					tlb->prepare(0, darwin_uml_static_vm_changes(0));
					require(gadgets::execute(built.program, gadget, *tlb) ==
						gadgets::Event::system_call);
					require(gadget.registers[7] == expected);
					require(gadget.nzcv == initial.nzcv);
				}

	for (const bool signed_divide : {false, true}) {
		const auto opcode = 0x9ac0'0800U |
			(signed_divide ? 0x400U : 0);
		for (const auto instruction : {
			opcode | (2U << 16) | (31U << 5) | 7U,
			opcode | (31U << 16) | (1U << 5) | 7U,
			opcode | (2U << 16) | (1U << 5) | 31U}) {
			arm64::CpuState cpu{};
			cpu.registers[1] = 21;
			cpu.registers[2] = 3;
			require(arm64::Dispatcher{}.execute(
				cpu, memory, instruction).reason ==
				arm64::StopReason::continued);
			if ((instruction & 31U) != 31)
				require(cpu.registers[7] == 0);
		}
	}
}

[[nodiscard]] std::uint64_t reference_variable_shift(
	std::uint64_t value, std::uint64_t encoded_shift, unsigned operation,
	bool wide)
{
	const unsigned width = wide ? 64 : 32;
	const auto mask = wide ? ~std::uint64_t{0} :
		std::uint64_t{0xffff'ffffU};
	value &= mask;
	const auto shift = static_cast<unsigned>(encoded_shift) & (width - 1);
	switch (operation) {
	case 0: return (value << shift) & mask;
	case 1: return value >> shift;
	case 2:
		return wide ? static_cast<std::uint64_t>(
			static_cast<std::int64_t>(value) >> shift) :
			static_cast<std::uint32_t>(
				static_cast<std::int32_t>(value) >> shift);
	default:
		return shift == 0 ? value :
			((value >> shift) | (value << (width - shift))) & mask;
	}
}

void variable_shifts()
{
	constexpr std::uint64_t base = 0x6000;
	const std::array values{
		std::uint64_t{0}, std::uint64_t{1},
		std::uint64_t{0x8000'0000U}, std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		~std::uint64_t{0}};
	const std::array shifts{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{31},
		std::uint64_t{32}, std::uint64_t{63}, std::uint64_t{64},
		std::uint64_t{65}, ~std::uint64_t{0}};
	auto tlb = std::make_unique<gadgets::Tlb>();
	NullMemory memory;
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 4; ++operation)
			for (const auto value : values)
				for (const auto shift : shifts) {
					const std::uint32_t instruction =
						0x1ac0'2000U |
						(wide ? 0x8000'0000U : 0) |
						(operation << 10) |
						(2U << 16) | (1U << 5) | 7U;
					const auto expected = reference_variable_shift(
						value, shift, operation, wide);
					arm64::CpuState initial{};
					initial.pc = base;
					initial.nzcv = 0xf000'0000U;
					initial.registers[1] = value;
					initial.registers[2] = shift;
					auto interpreted = initial;
					require(arm64::Dispatcher{}.execute(
						interpreted, memory, instruction).reason ==
						arm64::StopReason::continued);
					require(interpreted.registers[7] == expected);
					require(interpreted.nzcv == initial.nzcv);
					const std::array stream{instruction, 0xd400'0001U};
					const auto built = gadgets::Builder{}.build(base, stream);
					require(built.complete);
					auto gadget = initial;
					tlb->prepare(0, darwin_uml_static_vm_changes(0));
					require(gadgets::execute(built.program, gadget, *tlb) ==
						gadgets::Event::system_call);
					require(gadget.registers[7] == expected);
					require(gadget.nzcv == initial.nzcv);
				}
}

} // namespace

int main()
{
	carry_arithmetic();
	division();
	variable_shifts();
	return 0;
}
