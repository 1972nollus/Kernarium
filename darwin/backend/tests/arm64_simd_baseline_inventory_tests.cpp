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

extern "C" const std::uint32_t uml_arm64_neon_inventory_begin[];
extern "C" const std::uint32_t uml_arm64_neon_inventory_end[];

namespace {

constexpr std::uint64_t pc = 0x7a0'0000;

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

class ZeroMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t, std::span<std::byte> bytes) override
	{
		std::ranges::fill(bytes, std::byte{});
		return true;
	}
	[[nodiscard]] bool write(
		std::uint64_t, std::span<const std::byte>) override
	{
		return true;
	}
};

[[nodiscard]] arm64::CpuState initial_state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = index * 0x0102'0304'0506'0708ULL;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				reg * 29U + byte * 17U + 3U);
	cpu.sp = 0x1000;
	cpu.pc = pc;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0;
	cpu.fpsr = 0;
	return cpu;
}

[[nodiscard]] bool core_equal(const arm64::CpuState &expected,
	const arm64::CpuState &actual)
{
	if (actual.registers != expected.registers || actual.sp != expected.sp ||
	    actual.nzcv != expected.nzcv || actual.fpcr != expected.fpcr ||
	    actual.fpsr != expected.fpsr)
		return false;
	for (unsigned reg = 0; reg < expected.vectors.size(); ++reg)
		if (actual.vectors[reg].bytes != expected.vectors[reg].bytes)
			return false;
	return true;
}

void complete_baseline_inventory()
{
	ZeroMemory memory;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	unsigned failures = 0;
	for (auto encoded = uml_arm64_neon_inventory_begin;
	     encoded != uml_arm64_neon_inventory_end; ++encoded) {
		auto expected = initial_state();
		const auto interpreted = arm64::Dispatcher{}.execute(
			expected, memory, *encoded);
		if (interpreted.reason != arm64::StopReason::continued) {
			std::fprintf(stderr, "interpreter rejected baseline %#x\n", *encoded);
			++failures;
			continue;
		}

		const std::array stream{*encoded, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(pc, stream);
		if (!built.complete) {
			std::fprintf(stderr, "production builder rejected baseline %#x\n",
				*encoded);
			++failures;
			continue;
		}
		auto actual = initial_state();
		const auto event = gadgets::execute(built.program, actual, *tlb);
		if (event == gadgets::Event::illegal_instruction ||
		    event == gadgets::Event::fatal) {
			std::fprintf(stderr, "production gadget rejected baseline %#x\n",
				*encoded);
			++failures;
			continue;
		}
		if (event == gadgets::Event::system_call &&
		    !core_equal(expected, actual)) {
			std::fprintf(stderr, "production mismatch for baseline %#x\n",
				*encoded);
			++failures;
		}
	}
	require(failures == 0);
}

} // namespace

int main()
{
	complete_baseline_inventory();
	return 0;
}
