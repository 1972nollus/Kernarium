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

constexpr std::uint64_t base = 0xc000;

[[nodiscard]] constexpr std::uint32_t instruction(
	bool wide, unsigned operation, unsigned halfword, unsigned immediate,
	unsigned rd = 7)
{
	return 0x1280'0000U | (wide ? 0x8000'0000U : 0) |
		(operation << 29) | (halfword << 21) | (immediate << 5) | rd;
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
	require(interpreted.sp == initial.sp);

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
	require(gadget.sp == initial.sp);
}

void move_wide()
{
	const std::array immediates{0U, 1U, 0x7fffU, 0x8000U, 0xffffU};
	const std::array old_values{
		std::uint64_t{0}, std::uint64_t{0x0123'4567'89ab'cdefULL},
		~std::uint64_t{0}};
	for (const bool wide : {false, true})
		for (const unsigned operation : {0U, 2U, 3U})
			for (unsigned halfword = 0; halfword < (wide ? 4U : 2U);
			     ++halfword)
				for (const auto immediate : immediates)
					for (const auto old : old_values) {
						const unsigned shift = halfword * 16;
						const auto field = std::uint64_t{immediate} << shift;
						std::uint64_t expected;
						if (operation == 0)
							expected = ~field;
						else if (operation == 2)
							expected = field;
						else
							expected = (old &
								~(std::uint64_t{0xffff} << shift)) | field;
						if (!wide)
							expected &= 0xffff'ffffU;
						arm64::CpuState initial{};
						initial.nzcv = 0xf000'0000U;
						initial.sp = 0xfeed'face'feed'faceULL;
						initial.registers[7] = old;
						check(instruction(wide, operation, halfword,
							immediate), initial, expected);
					}

	arm64::CpuState initial{};
	initial.nzcv = 0x5000'0000U;
	initial.registers[7] = 0x0123'4567'89ab'cdefULL;
	check(instruction(true, 2, 3, 0xabcd, 31), initial, 0, 31);
}

void undefined_encodings()
{
	NullMemory memory;
	const std::array encodings{
		instruction(false, 0, 2, 1),
		instruction(false, 2, 3, 1),
		instruction(false, 3, 2, 1),
		instruction(true, 1, 0, 1),
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
	move_wide();
	undefined_encodings();
	return 0;
}
