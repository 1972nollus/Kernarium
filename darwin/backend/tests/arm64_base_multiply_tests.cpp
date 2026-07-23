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

constexpr std::uint64_t base = 0x8000;

void check(std::uint32_t instruction, const arm64::CpuState &initial,
	   std::uint64_t expected, unsigned destination = 7)
{
	NullMemory memory;
	auto interpreted = initial;
	interpreted.pc = base;
	require(arm64::Dispatcher{}.execute(interpreted, memory, instruction).reason ==
		arm64::StopReason::continued);
	if (destination != 31)
		require(interpreted.registers[destination] == expected);
	require(interpreted.nzcv == initial.nzcv);

	const std::array stream{instruction, 0xd400'0001U};
	const auto built = gadgets::Builder{}.build(base, stream);
	require(built.complete);
	auto gadget = initial;
	gadget.pc = base;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::system_call);
	if (destination != 31)
		require(gadget.registers[destination] == expected);
	require(gadget.nzcv == initial.nzcv);
}

[[nodiscard]] constexpr std::uint32_t regular_instruction(
	bool wide, bool subtract, unsigned rn = 1, unsigned rm = 2,
	unsigned ra = 3, unsigned rd = 7)
{
	return 0x1b00'0000U | (wide ? 0x8000'0000U : 0) |
		(rm << 16) | (subtract ? 0x8000U : 0) | (ra << 10) |
		(rn << 5) | rd;
}

[[nodiscard]] constexpr std::uint32_t long_instruction(
	bool unsigned_values, bool subtract, unsigned rn = 1, unsigned rm = 2,
	unsigned ra = 3, unsigned rd = 7)
{
	return (unsigned_values ? 0x9ba0'0000U : 0x9b20'0000U) |
		(rm << 16) | (subtract ? 0x8000U : 0) | (ra << 10) |
		(rn << 5) | rd;
}

[[nodiscard]] constexpr std::uint32_t high_instruction(
	bool unsigned_values, unsigned rn = 1, unsigned rm = 2, unsigned rd = 7,
	unsigned reserved_ra = 31)
{
	return (unsigned_values ? 0x9bc0'0000U : 0x9b40'0000U) |
		(rm << 16) | (reserved_ra << 10) | (rn << 5) | rd;
}

void regular_multiply_add_subtract()
{
	const std::array values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2},
		std::uint64_t{0x7fff'ffffU}, std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		~std::uint64_t{0}};
	const std::array addends{
		std::uint64_t{0}, std::uint64_t{1},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		~std::uint64_t{0}};
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const auto left_value : values)
				for (const auto right_value : values)
					for (const auto addend_value : addends) {
						const auto mask = wide ? ~std::uint64_t{0} :
							std::uint64_t{0xffff'ffffU};
						const auto product = (left_value & mask) *
							(right_value & mask);
						const auto expected = (subtract ?
							addend_value - product :
							addend_value + product) & mask;
						arm64::CpuState initial{};
						initial.nzcv = 0xa000'0000U;
						initial.registers[1] = left_value;
						initial.registers[2] = right_value;
						initial.registers[3] = addend_value;
						check(regular_instruction(wide, subtract),
							initial, expected);
					}

	/* MUL and MNEG aliases use the architectural zero register as Ra. */
	arm64::CpuState initial{};
	initial.nzcv = 0x5000'0000U;
	initial.registers[1] = 7;
	initial.registers[2] = 9;
	check(regular_instruction(true, false, 1, 2, 31), initial, 63);
	check(regular_instruction(true, true, 1, 2, 31), initial, -63ULL);
	check(regular_instruction(false, false, 31, 2, 31), initial, 0);
	check(regular_instruction(true, false, 1, 31, 31), initial, 0);
	check(regular_instruction(true, false, 1, 2, 31, 31), initial,
		0, 31);
}

void long_multiply_add_subtract()
{
	const std::array values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2},
		std::uint64_t{0x7fff'ffffU}, std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0xffff'ffff'8000'0001ULL}};
	const std::array addends{
		std::uint64_t{0}, std::uint64_t{1},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		~std::uint64_t{0}};
	for (const bool unsigned_values : {false, true})
		for (const bool subtract : {false, true})
			for (const auto left_value : values)
				for (const auto right_value : values)
					for (const auto addend : addends) {
						std::uint64_t product;
						if (unsigned_values) {
							product = static_cast<std::uint64_t>(
								static_cast<std::uint32_t>(left_value)) *
								static_cast<std::uint32_t>(right_value);
						} else {
							product = static_cast<std::uint64_t>(
								static_cast<std::int64_t>(
									static_cast<std::int32_t>(left_value)) *
								static_cast<std::int32_t>(right_value));
						}
						const auto expected = subtract ? addend - product :
							addend + product;
						arm64::CpuState initial{};
						initial.nzcv = 0x3000'0000U;
						initial.registers[1] = left_value;
						initial.registers[2] = right_value;
						initial.registers[3] = addend;
						check(long_instruction(unsigned_values, subtract),
							initial, expected);
					}

	/* SMULL/SMNEGL and UMULL/UMNEGL aliases have Ra == 31. */
	arm64::CpuState initial{};
	initial.nzcv = 0xc000'0000U;
	initial.registers[1] = 0xffff'ffffU;
	initial.registers[2] = 2;
	check(long_instruction(false, false, 1, 2, 31), initial, -2ULL);
	check(long_instruction(false, true, 1, 2, 31), initial, 2);
	check(long_instruction(true, false, 1, 2, 31), initial,
		0x1'ffff'fffeULL);
	check(long_instruction(true, true, 1, 2, 31), initial,
		-0x1'ffff'fffeULL);
}

void multiply_high()
{
	const std::array values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0xffff'ffff'0000'0000ULL},
		~std::uint64_t{0}};
	for (const bool unsigned_values : {false, true})
		for (const auto left : values)
			for (const auto right : values) {
				std::uint64_t expected;
				if (unsigned_values) {
					const unsigned __int128 product =
						static_cast<unsigned __int128>(left) * right;
					expected = static_cast<std::uint64_t>(product >> 64);
				} else {
					const __int128 product = static_cast<__int128>(
						static_cast<std::int64_t>(left)) *
						static_cast<std::int64_t>(right);
					expected = static_cast<std::uint64_t>(
						static_cast<unsigned __int128>(product) >> 64);
				}
				arm64::CpuState initial{};
				initial.nzcv = 0x9000'0000U;
				initial.registers[1] = left;
				initial.registers[2] = right;
				check(high_instruction(unsigned_values), initial, expected);
			}
}

void reserved_encodings()
{
	NullMemory memory;
	const std::array instructions{
			0x1b20'0c27U, // 32-bit long operation
			0x9b40'fc27U, // SMULH with subtract bit set
			0x9bc0'0027U, // UMULH with reserved Ra != 11111
			/* operation 3 is FEAT_CPA MADDPT/MSUBPT, covered by the
			 * dedicated split test rather than this legacy reserved matrix.
			 */
			0xbb00'0027U, // nonzero op54
	};
	for (const auto instruction : instructions) {
		arm64::CpuState cpu{};
		cpu.pc = base;
		require(arm64::Dispatcher{}.execute(cpu, memory, instruction).reason ==
			arm64::StopReason::illegal_instruction);
		const std::array stream{instruction, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(base, stream);
		require(built.complete);
		auto tlb = std::make_unique<gadgets::Tlb>();
		tlb->prepare(0, darwin_uml_static_vm_changes(0));
		require(gadgets::execute(built.program, cpu, *tlb) ==
			gadgets::Event::illegal_instruction);
	}
}

} // namespace

int main()
{
	regular_multiply_add_subtract();
	long_multiply_add_subtract();
	multiply_high();
	reserved_encodings();
	return 0;
}
