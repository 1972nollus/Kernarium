// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <bit>
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

[[nodiscard]] std::uint64_t reverse_groups(
	std::uint64_t value, unsigned width, unsigned group_bytes)
{
	std::uint64_t result = 0;
	for (unsigned group = 0; group < width / 8; group += group_bytes)
		for (unsigned byte = 0; byte < group_bytes; ++byte) {
			const auto source = group + byte;
			const auto destination = group + group_bytes - 1 - byte;
			result |= ((value >> (source * 8)) & 0xffU) <<
				(destination * 8);
		}
	return result;
}

[[nodiscard]] std::uint64_t reference(
	std::uint64_t value, unsigned operation, bool wide)
{
	const unsigned width = wide ? 64 : 32;
	const auto mask = wide ? ~std::uint64_t{0} :
		std::uint64_t{0xffff'ffffU};
	value &= mask;
	if (operation == 0) {
		std::uint64_t result = 0;
		for (unsigned bit = 0; bit < width; ++bit)
			result |= ((value >> bit) & 1U) << (width - 1 - bit);
		return result;
	}
	if (operation <= 3)
		return reverse_groups(value, width, 1U << operation);
	if (wide) {
		const auto count_value = operation == 5 && (value >> 63) != 0 ?
			~value : value;
		return std::countl_zero(count_value) - (operation == 5 ? 1 : 0);
	}
	const auto narrowed = static_cast<std::uint32_t>(value);
	const auto count_value = operation == 5 && (narrowed >> 31) != 0 ?
		~narrowed : narrowed;
	return std::countl_zero(count_value) - (operation == 5 ? 1 : 0);
}

void bit_reverse_and_count()
{
	constexpr std::uint64_t base = 0x7000;
	const std::array values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x80},
		std::uint64_t{0x8000'0000U}, std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		~std::uint64_t{0}};
	auto tlb = std::make_unique<gadgets::Tlb>();
	NullMemory memory;
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation <= 5; ++operation) {
			if (!wide && operation == 3)
				continue;
			for (const auto value : values) {
				const std::uint32_t instruction = 0x5ac0'0000U |
					(wide ? 0x8000'0000U : 0) | (operation << 10) |
					(1U << 5) | 7U;
				const auto expected = reference(value, operation, wide);
				arm64::CpuState initial{};
				initial.pc = base;
				initial.nzcv = 0xf000'0000U;
				initial.registers[1] = value;
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

	arm64::CpuState cpu{};
	cpu.pc = base;
	require(arm64::Dispatcher{}.execute(
		cpu, memory, 0x5ac0'0c27U).reason ==
		arm64::StopReason::unsupported_instruction); // reserved 32-bit REV64
	const std::array reserved{0x5ac0'0c27U, 0xd400'0001U};
	const auto reserved_built = gadgets::Builder{}.build(base, reserved);
	if (reserved_built.complete) {
		auto reserved_tlb = std::make_unique<gadgets::Tlb>();
		reserved_tlb->prepare(0, darwin_uml_static_vm_changes(0));
		require(gadgets::execute(reserved_built.program, cpu,
			*reserved_tlb) == gadgets::Event::fatal);
	}

	/* Architectural zero-register source and destination behavior. */
	for (const auto instruction : {0xdac0'03e7U, 0xdac0'003fU}) {
		cpu = {};
		cpu.pc = base;
		cpu.registers[1] = 0x0123'4567'89ab'cdefULL;
		require(arm64::Dispatcher{}.execute(
			cpu, memory, instruction).reason ==
			arm64::StopReason::continued);
		if ((instruction & 31U) != 31)
			require(cpu.registers[7] == 0);
		else
			require(cpu.registers[1] == 0x0123'4567'89ab'cdefULL);
	}
}

} // namespace

int main()
{
	bit_reverse_and_count();
	return 0;
}
