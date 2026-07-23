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

constexpr std::uint64_t base = 0x1'0000;

[[nodiscard]] arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0x0102'0304'0506'0708ULL ^ index;
	cpu.sp = 0xfeed'face'cafebeefULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	return cpu;
}

void require_unchanged(const arm64::CpuState &actual,
		       const arm64::CpuState &initial, std::uint64_t pc)
{
	require(actual.registers == initial.registers);
	require(actual.sp == initial.sp);
	require(actual.pc == pc);
	require(actual.nzcv == initial.nzcv);
	require(actual.fpcr == initial.fpcr);
	require(actual.fpsr == initial.fpsr);
	require(actual.tls == initial.tls);
}

void check(std::uint32_t instruction)
{
	NullMemory memory;
	const auto initial = state();
	auto interpreted = initial;
	interpreted.pc = base;
	require(arm64::Dispatcher{}.execute(
		interpreted, memory, instruction).reason ==
		arm64::StopReason::continued);
	require_unchanged(interpreted, initial, base + 4);

	const std::array stream{instruction, 0xd400'0001U};
	const auto built = gadgets::Builder{}.build(base, stream);
	require(built.complete);
	auto gadget = initial;
	gadget.pc = base;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::system_call);
	require_unchanged(gadget, initial, base + 8);
}

void standard_barriers()
{
	for (unsigned option = 0; option < 16; ++option) {
		check(0xd503'309fU | (option << 8)); // DSB, including SSBB/PSSBB
		check(0xd503'30bfU | (option << 8)); // DMB
		check(0xd503'30dfU | (option << 8)); // ISB
	}
}

void extension_barriers()
{
	for (unsigned scope = 0; scope < 4; ++scope)
		check(0xd503'323fU | (scope << 10)); // DSB <scope>nXS
	check(0xd503'30ffU); // SB
}

} // namespace

int main()
{
	standard_barriers();
	extension_barriers();
	return 0;
}
