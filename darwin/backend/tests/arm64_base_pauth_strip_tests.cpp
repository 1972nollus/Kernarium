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

constexpr std::uint64_t pc = 0x4c0'0000;

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

[[nodiscard]] constexpr std::uint32_t xpac(
	bool data, unsigned destination, unsigned source = 31)
{
	return 0xdac1'4000U | (static_cast<unsigned>(data) << 10) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint64_t stripped(
	std::uint64_t source, bool data)
{
	const bool upper = (source & (std::uint64_t{1} << 55)) != 0;
	if (data && !upper)
		return (source & 0xff00'7fff'ffff'ffffULL) |
			(upper ? 0x00ff'8000'0000'0000ULL : 0);
	return (source & 0x0000'7fff'ffff'ffffULL) |
		(upper ? 0xffff'8000'0000'0000ULL : 0);
}

[[nodiscard]] arm64::CpuState state(std::uint64_t value, unsigned destination)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0x1020'3040'5060'7080ULL ^ index;
	if (destination < cpu.registers.size())
		cpu.registers[destination] = value;
	cpu.sp = 0x1234'5678'9abc'def0ULL;
	cpu.nzcv = 0xa000'0000U;
	return cpu;
}

void check(std::uint32_t instruction, const arm64::CpuState &initial,
	unsigned destination, std::uint64_t expected)
{
	NullMemory memory;
	auto interpreted = initial;
	interpreted.pc = pc;
	require(arm64::Dispatcher{}.execute(
		interpreted, memory, instruction).reason ==
		arm64::StopReason::continued);
	require(interpreted.pc == pc + 4);
	if (destination < 31)
		require(interpreted.registers[destination] == expected);
	else
		require(interpreted.registers == initial.registers);

	const std::array stream{instruction, 0xd400'0001U};
	const auto built = gadgets::Builder{}.build(pc, stream);
	require(built.complete);
	auto gadget = initial;
	gadget.pc = pc;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::system_call);
	require(gadget.pc == pc + 8);
	if (destination < 31) {
		require(gadget.registers[destination] == expected);
	} else
		require(gadget.registers == initial.registers);
}

void require_illegal(std::uint32_t instruction)
{
	NullMemory memory;
	const auto initial = state(0xab55'1234'5678'9abcULL, 3);
	auto interpreted = initial;
	interpreted.pc = pc;
	require(arm64::Dispatcher{}.execute(
		interpreted, memory, instruction).reason ==
		arm64::StopReason::illegal_instruction);
	require(interpreted.pc == pc && interpreted.registers == initial.registers);

	const std::array stream{instruction, 0xd400'0001U};
	const auto built = gadgets::Builder{}.build(pc, stream);
	require(built.complete);
	auto gadget = initial;
	gadget.pc = pc;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::illegal_instruction);
	require(gadget.pc == pc && gadget.registers == initial.registers);
}

void integer_forms()
{
	constexpr std::array values{
		0xab55'1234'5678'9abcULL,
		0x12d5'fedc'ba98'7654ULL,
		0xff00'0000'0000'0000ULL,
		0x00ff'ffff'ffff'ffffULL,
		0x1234'8000'0000'0001ULL,
	};
	for (const bool data : {false, true})
		for (unsigned destination = 0; destination < 32; ++destination)
			for (const auto value : values) {
				const auto initial = state(value, destination);
				const auto source = destination == 31 ? 0 : value;
				check(xpac(data, destination), initial, destination,
					stripped(source, data));
				if (data) {
					auto tagged = initial;
					tagged.set_mte_configuration(true, 0);
					check(xpac(true, destination), tagged, destination,
						stripped(source, true));
				}
			}
}

void link_register_form()
{
	for (const auto value : {0xab55'1234'5678'9abcULL,
		0x12d5'fedc'ba98'7654ULL, 0x0000'ffff'ffff'ffffULL}) {
		const auto initial = state(value, 30);
		check(0xd503'20ffU, initial, 30, stripped(value, false));
	}
}

void reserved_source()
{
	for (const bool data : {false, true})
		for (unsigned source = 0; source < 31; ++source)
			for (const unsigned destination : {0U, 17U, 31U})
				require_illegal(xpac(data, destination, source));
}

void production_specialization()
{
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	const auto i = gadgets::Builder{}.build(pc, std::array{xpac(false, 3)});
	const auto d = gadgets::Builder{}.build(pc, std::array{xpac(true, 3)});
	const auto lr = gadgets::Builder{}.build(pc, std::array{0xd503'20ffU});
	const auto discard = gadgets::Builder{}.build(
		pc, std::array{xpac(false, 31)});
	const auto nop = gadgets::Builder{}.build(pc, std::array{0xd503'201fU});
	require(i.program.words().front() != d.program.words().front());
	require(i.program.words().front() != lr.program.words().front());
	require(d.program.words().front() != lr.program.words().front());
	require(discard.program.words().front() == nop.program.words().front());
#endif
}

} // namespace

int main()
{
	integer_forms();
	link_register_form();
	reserved_source();
	production_specialization();
	return 0;
}
