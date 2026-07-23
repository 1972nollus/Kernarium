// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

static_assert(static_cast<int>(gadgets::Event::breakpoint) ==
	DARWIN_UML_EVENT_BREAKPOINT);

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

constexpr std::uint64_t base = 0x3'0000;

[[nodiscard]] arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0xa5a5'5a5a'1234'0000ULL | index;
	cpu.sp = 0xfeed'face'cafebeefULL;
	cpu.nzcv = 0xb000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.set_mte_configuration(true, 0x8421U);
	cpu.set_mte_generator(true, 0xace1U, 7, 0xfeed'face'0123'4567ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0xa1b2'c3d4U;
	cpu.exclusive_address = 0x7654'3000ULL;
	cpu.exclusive_key = 0x1234U;
	cpu.exclusive_value = 0xa5a5'a5a5'a5a5'a5a5ULL;
	cpu.exclusive_value2 = 0x5a5a'5a5a'5a5a'5a5aULL;
	cpu.exclusive_version = 19;
	cpu.exclusive_size = 16;
	return cpu;
}

void require_state(const arm64::CpuState &actual,
		   const arm64::CpuState &initial, std::uint64_t pc)
{
	require(actual.registers == initial.registers && actual.sp == initial.sp &&
		actual.pc == pc && actual.nzcv == initial.nzcv &&
		actual.fpcr == initial.fpcr &&
		actual.fpsr == initial.fpsr && actual.tls == initial.tls &&
		actual.mte_enabled == initial.mte_enabled &&
		actual.mte_exclude == initial.mte_exclude &&
		actual.mte_tag_choices == initial.mte_tag_choices &&
		actual.mte_random_tags == initial.mte_random_tags &&
		actual.mte_seed == initial.mte_seed &&
		actual.mte_tag == initial.mte_tag &&
		actual.mte_random_state == initial.mte_random_state &&
		actual.cpa_enabled == initial.cpa_enabled &&
		actual.cpa_multiply_enabled == initial.cpa_multiply_enabled &&
		actual.accdata == initial.accdata &&
		actual.exclusive_address == initial.exclusive_address &&
		actual.exclusive_key == initial.exclusive_key &&
		actual.exclusive_value == initial.exclusive_value &&
		actual.exclusive_value2 == initial.exclusive_value2 &&
		actual.exclusive_version == initial.exclusive_version &&
		actual.exclusive_size == initial.exclusive_size &&
		actual.deferred_vectors == initial.deferred_vectors &&
		actual.deferred_fpcr == initial.deferred_fpcr &&
		actual.deferred_fpsr == initial.deferred_fpsr &&
		actual.fp_loaded == initial.fp_loaded &&
		actual.fp_dirty == initial.fp_dirty);
	for (unsigned index = 0; index < actual.vectors.size(); ++index)
		require(actual.vectors[index].bytes == initial.vectors[index].bytes);
}

void check_interpreter(std::uint32_t instruction, arm64::StopReason reason,
		       std::size_t count, bool advance)
{
	NullMemory memory;
	const auto initial = state();
	auto cpu = initial;
	cpu.pc = base;
	const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
	require(result.reason == reason);
	require(result.address == base);
	require(result.instruction == instruction);
	require(result.instructions == count);
	require_state(cpu, initial, advance ? base + 4 : base);
}

void check_gadget(std::uint32_t instruction, gadgets::Event event,
		  bool advance, gadgets::Tlb &tlb)
{
	const auto built = gadgets::Builder{}.build(
		base, std::array{instruction});
	require(built.complete);
	require(built.program.instruction_count() == 1);
	const auto initial = state();
	auto cpu = initial;
	cpu.pc = base;
	tlb.prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, cpu, tlb) == event);
	require_state(cpu, initial, advance ? base + 4 : base);
}

[[nodiscard]] constexpr std::uint32_t with_immediate(
	std::uint32_t base_instruction, unsigned immediate)
{
	return base_instruction | (immediate << 5);
}

void synchronous_calls()
{
	auto tlb = std::make_unique<gadgets::Tlb>();
	for (unsigned immediate = 0; immediate <= 0xffffU; ++immediate) {
		const auto svc = with_immediate(0xd400'0001U, immediate);
		check_interpreter(svc, arm64::StopReason::system_call, 1, true);
		check_gadget(svc, gadgets::Event::system_call, true, *tlb);

		const auto brk = with_immediate(0xd420'0000U, immediate);
		check_interpreter(brk, arm64::StopReason::breakpoint, 1, false);
		check_gadget(brk, gadgets::Event::breakpoint, false, *tlb);

		const auto udf = immediate;
		check_interpreter(
			udf, arm64::StopReason::illegal_instruction, 0, false);
		check_gadget(
			udf, gadgets::Event::illegal_instruction, false, *tlb);
	}
}

void el0_undefined_exceptions()
{
	constexpr std::array bases{
		0xd400'0002U, // HVC
		0xd400'0003U, // SMC
		0xd440'0000U, // HLT in the non-Debug execution context
		0xd4a0'0001U, // DCPS1 in the non-Debug execution context
		0xd4a0'0002U, // DCPS2 in the non-Debug execution context
		0xd4a0'0003U, // DCPS3 in the non-Debug execution context
	};
	for (const auto instruction_base : bases)
		for (unsigned immediate = 0; immediate <= 0xffffU; ++immediate)
			check_interpreter(with_immediate(instruction_base, immediate),
				arm64::StopReason::illegal_instruction, 0, false);

	auto tlb = std::make_unique<gadgets::Tlb>();
	for (const auto instruction_base : bases)
		for (const unsigned immediate : {0U, 1U, 0x7fffU, 0xffffU})
			check_gadget(with_immediate(instruction_base, immediate),
				gadgets::Event::illegal_instruction, false, *tlb);

	check_interpreter(0xd6bf'03e0U, arm64::StopReason::illegal_instruction,
		0, false); // DRPS in the non-Debug EL0 execution context
	check_gadget(0xd6bf'03e0U, gadgets::Event::illegal_instruction,
		false, *tlb);
	for (const auto instruction : {
		0xd69f'03e0U, // ERET is Undefined at EL0.
		0xd69f'0bffU, // ERETAA is Undefined at EL0.
		0xd69f'0fffU, // ERETAB is Undefined at EL0.
	}) {
		check_interpreter(instruction, arm64::StopReason::illegal_instruction,
			0, false);
		check_gadget(instruction, gadgets::Event::illegal_instruction,
			false, *tlb);
	}
}

} // namespace

int main()
{
	synchronous_calls();
	el0_undefined_exceptions();
	return 0;
}
