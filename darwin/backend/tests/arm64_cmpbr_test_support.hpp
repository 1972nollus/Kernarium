// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>

namespace cmpbr_test {

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

constexpr std::uint64_t base = 0x6400'0000;

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

class RejectingMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t, std::span<std::byte>) override
	{
		++reads;
		return false;
	}
	[[nodiscard]] bool write(
		std::uint64_t, std::span<const std::byte>) override
	{
		++writes;
		return false;
	}
	std::size_t reads{};
	std::size_t writes{};
};

[[nodiscard]] inline arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0x1122'3344'5566'7700ULL + index * 0x101U;
	cpu.sp = 0xcafe'0000'0000'0040ULL;
	cpu.nzcv = 0xd000'0000U;
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

inline void require_state(const arm64::CpuState &actual,
	const arm64::CpuState &expected)
{
	require(actual.registers == expected.registers && actual.sp == expected.sp &&
		actual.pc == expected.pc && actual.nzcv == expected.nzcv &&
		actual.fpcr == expected.fpcr && actual.fpsr == expected.fpsr &&
		actual.tls == expected.tls &&
		actual.mte_enabled == expected.mte_enabled &&
		actual.mte_exclude == expected.mte_exclude &&
		actual.mte_tag_choices == expected.mte_tag_choices &&
		actual.mte_random_tags == expected.mte_random_tags &&
		actual.mte_seed == expected.mte_seed &&
		actual.mte_tag == expected.mte_tag &&
		actual.mte_random_state == expected.mte_random_state &&
		actual.cpa_enabled == expected.cpa_enabled &&
		actual.cpa_multiply_enabled == expected.cpa_multiply_enabled &&
		actual.accdata == expected.accdata &&
		actual.exclusive_address == expected.exclusive_address &&
		actual.exclusive_key == expected.exclusive_key &&
		actual.exclusive_value == expected.exclusive_value &&
		actual.exclusive_value2 == expected.exclusive_value2 &&
		actual.exclusive_version == expected.exclusive_version &&
		actual.exclusive_size == expected.exclusive_size &&
		actual.deferred_vectors == expected.deferred_vectors &&
		actual.deferred_fpcr == expected.deferred_fpcr &&
		actual.deferred_fpsr == expected.deferred_fpsr &&
		actual.fp_loaded == expected.fp_loaded &&
		actual.fp_dirty == expected.fp_dirty);
	for (unsigned index = 0; index < actual.vectors.size(); ++index)
		require(actual.vectors[index].bytes == expected.vectors[index].bytes);
}

class Harness final {
public:
	Harness() : tlb_{std::make_unique<gadgets::Tlb>()}
	{
		tlb_->prepare(0, darwin_uml_static_vm_changes(0));
	}

	void check(std::uint32_t instruction, const arm64::CpuState &initial,
		bool take, std::int64_t displacement)
	{
		auto expected = initial;
		expected.pc = take ? static_cast<std::uint64_t>(
			static_cast<std::int64_t>(base) + displacement) : base + 4;

		auto interpreted = initial;
		interpreted.pc = base;
		const auto result = arm64::Dispatcher{}.execute(
			interpreted, memory_, instruction);
		require(result.reason == arm64::StopReason::continued &&
			result.instructions == 1 && result.address == base &&
			result.instruction == instruction && !result.write);
		require_state(interpreted, expected);

		const auto built = gadgets::Builder{}.build(base,
			std::span<const std::uint32_t>{&instruction, 1});
		require(built.complete && built.program.instruction_count() == 1);
		auto gadget = initial;
		gadget.pc = base;
		const auto event = gadgets::execute(built.program, gadget, *tlb_);
		require(event == gadgets::Event::continued ||
			event == gadgets::Event::interrupt);
		require_state(gadget, expected);
		require(memory_.reads == 0 && memory_.writes == 0);
	}

	void check_illegal(std::uint32_t instruction)
	{
		auto initial = state();
		initial.pc = base;
		auto interpreted = initial;
		const auto result = arm64::Dispatcher{}.execute(
			interpreted, memory_, instruction);
		require(result.reason == arm64::StopReason::illegal_instruction &&
			result.instructions == 0 && result.address == base &&
			result.instruction == instruction && !result.write);
		require_state(interpreted, initial);

		const auto built = gadgets::Builder{}.build(base,
			std::span<const std::uint32_t>{&instruction, 1});
		auto gadget = initial;
		const auto event = gadgets::execute(built.program, gadget, *tlb_);
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
		require(built.complete && event == gadgets::Event::illegal_instruction);
#else
		require(!built.complete && event == gadgets::Event::fatal);
#endif
		require_state(gadget, initial);
		require(memory_.reads == 0 && memory_.writes == 0);
	}

private:
	RejectingMemory memory_;
	std::unique_ptr<gadgets::Tlb> tlb_;
};

[[nodiscard]] constexpr std::int64_t displacement(unsigned imm9)
{
	const auto shifted = static_cast<std::uint64_t>(imm9 & 0x1ffU) << 2;
	return (shifted & 0x400U) != 0 ?
		static_cast<std::int64_t>(shifted | ~std::uint64_t{0x7ffU}) :
		static_cast<std::int64_t>(shifted);
}

} // namespace cmpbr_test
