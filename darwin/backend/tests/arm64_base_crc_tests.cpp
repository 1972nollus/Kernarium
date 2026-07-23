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

constexpr std::uint64_t base = 0xf000;

[[nodiscard]] constexpr std::uint32_t instruction(
	unsigned operation, unsigned rn = 1, unsigned rm = 2, unsigned rd = 7,
	bool wrong_width = false)
{
	const bool wide = (operation & 3U) == 3U;
	return 0x1ac0'4000U | ((wide != wrong_width) ? 0x8000'0000U : 0) |
		(operation << 10) | (rm << 16) | (rn << 5) | rd;
}

[[nodiscard]] std::uint32_t reference(
	std::uint32_t accumulator, std::uint64_t data, unsigned bytes,
	bool castagnoli)
{
	const std::uint32_t polynomial = castagnoli ?
		0x82f6'3b78U : 0xedb8'8320U;
	for (unsigned byte = 0; byte < bytes; ++byte) {
		accumulator ^= static_cast<std::uint8_t>(data >> (byte * 8));
		for (unsigned bit = 0; bit < 8; ++bit)
			accumulator = (accumulator >> 1) ^
				((accumulator & 1U) != 0 ? polynomial : 0);
	}
	return accumulator;
}

void check(std::uint32_t encoded, const arm64::CpuState &initial,
	   std::uint32_t expected, unsigned destination = 7)
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

void crc32_operations()
{
	const std::array accumulators{
		std::uint32_t{0}, std::uint32_t{1}, std::uint32_t{0x8000'0000U},
		std::uint32_t{0xffff'ffffU}, std::uint32_t{0x1234'5678U}};
	const std::array data_values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0xff},
		std::uint64_t{0x1234'5678U},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
		~std::uint64_t{0}};
	for (unsigned operation = 0; operation < 8; ++operation) {
		const unsigned bytes = 1U << (operation & 3U);
		for (const auto accumulator : accumulators)
			for (const auto data : data_values) {
				arm64::CpuState initial{};
				initial.nzcv = 0xf000'0000U;
				initial.registers[1] =
					0xfeed'face'0000'0000ULL | accumulator;
				initial.registers[2] = data;
				check(instruction(operation), initial,
					reference(accumulator, data, bytes, operation >= 4));
			}
	}

	arm64::CpuState initial{};
	initial.nzcv = 0x5000'0000U;
	initial.registers[1] = 0x1234'5678U;
	initial.registers[2] = 0x89ab'cdefU;
	check(instruction(2, 31, 2), initial,
		reference(0, initial.registers[2], 4, false));
	check(instruction(6, 1, 31), initial,
		reference(0x1234'5678U, 0, 4, true));
	check(instruction(7, 1, 2, 31), initial, 0, 31);
}

void undefined_widths()
{
	NullMemory memory;
	for (unsigned operation = 0; operation < 8; ++operation) {
		const auto encoded = instruction(operation, 1, 2, 7, true);
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
	crc32_operations();
	undefined_widths();
	return 0;
}
