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

constexpr std::uint64_t base = 0xe000;

[[nodiscard]] constexpr std::uint32_t instruction(
	bool wide, unsigned shift, unsigned rn = 1, unsigned rm = 2,
	unsigned rd = 7)
{
	return 0x1380'0000U | (wide ? 0x8040'0000U : 0) |
		(rm << 16) | (shift << 10) | (rn << 5) | rd;
}

void check(std::uint32_t encoded, const arm64::CpuState &initial,
	   std::uint64_t expected, unsigned destination = 7)
{
	NullMemory memory;
	auto interpreted = initial;
	interpreted.pc = base;
	require(arm64::Dispatcher{}.execute(interpreted, memory, encoded).reason ==
		arm64::StopReason::continued);
	if (destination != 31)
		require(interpreted.registers[destination] == expected);
	require(interpreted.nzcv == initial.nzcv);

	const std::array stream{encoded, 0xd400'0001U};
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

void extract_and_rotate()
{
	const std::array pairs{
		std::array{std::uint64_t{0}, std::uint64_t{0}},
		std::array{~std::uint64_t{0}, std::uint64_t{0}},
		std::array{std::uint64_t{0}, ~std::uint64_t{0}},
		std::array{std::uint64_t{0x0123'4567'89ab'cdefULL},
			std::uint64_t{0xfedc'ba98'7654'3210ULL}}};
	for (const bool wide : {false, true}) {
		const unsigned width = wide ? 64 : 32;
		const auto mask = wide ? ~std::uint64_t{0} :
			std::uint64_t{0xffff'ffffU};
		for (unsigned shift = 0; shift < width; ++shift)
			for (const auto &pair : pairs) {
				const auto low = pair[1] & mask;
				const auto high = pair[0] & mask;
				const auto expected = shift == 0 ? low :
					(low >> shift) | ((high << (width - shift)) & mask);
				arm64::CpuState initial{};
				initial.nzcv = 0xf000'0000U;
				initial.registers[1] = pair[0];
				initial.registers[2] = pair[1];
				check(instruction(wide, shift), initial, expected);
			}

		/* ROR is EXTR with equal sources. */
		arm64::CpuState initial{};
		initial.nzcv = 0x5000'0000U;
		initial.registers[1] = 0x8000'0000'0000'0001ULL;
		for (const unsigned shift : {0U, 1U, width - 1}) {
			const auto value = initial.registers[1] & mask;
			const auto expected = shift == 0 ? value :
				(value >> shift) | ((value << (width - shift)) & mask);
			check(instruction(wide, shift, 1, 1), initial, expected);
		}
		check(instruction(wide, 0, 31, 2), initial, 0);
		check(instruction(wide, 0, 1, 31), initial, 0);
		check(instruction(wide, 0, 1, 2, 31), initial, 0, 31);
	}
}

void undefined_encodings()
{
	NullMemory memory;
	const std::array encodings{
		instruction(false, 32),
		static_cast<std::uint32_t>(instruction(false, 0) | 0x0040'0000U),
		static_cast<std::uint32_t>(instruction(true, 0) & ~0x0040'0000U),
	};
	for (const auto encoded : encodings) {
		arm64::CpuState cpu{};
		cpu.pc = base;
		require(arm64::Dispatcher{}.execute(cpu, memory, encoded).reason ==
			arm64::StopReason::unsupported_instruction);
		const std::array stream{encoded, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(base, stream);
		if (built.complete) {
			auto tlb = std::make_unique<gadgets::Tlb>();
			tlb->prepare(0, darwin_uml_static_vm_changes(0));
			require(gadgets::execute(built.program, cpu, *tlb) ==
				gadgets::Event::fatal);
		}
	}
}

} // namespace

int main()
{
	extract_and_rotate();
	undefined_encodings();
	return 0;
}
