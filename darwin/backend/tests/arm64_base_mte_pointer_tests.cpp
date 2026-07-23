// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <source_location>

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x5b0'0000;

void require(bool condition,
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

[[nodiscard]] constexpr std::uint32_t gmi(
	unsigned destination, unsigned address, unsigned mask)
{
	return 0x9ac0'1400U | ((mask & 31U) << 16) |
		((address & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t subp(bool flags,
	unsigned destination, unsigned first, unsigned second)
{
	return (flags ? 0xbac0'0000U : 0x9ac0'0000U) |
		((second & 31U) << 16) | ((first & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint64_t sign_extend_56(std::uint64_t value)
{
	constexpr auto sign = std::uint64_t{1} << 55;
	constexpr auto mask = (std::uint64_t{1} << 56) - 1;
	value &= mask;
	return (value ^ sign) - sign;
}

[[nodiscard]] constexpr std::uint32_t subtract_flags(
	std::uint64_t left, std::uint64_t right)
{
	const auto result = left - right;
	const bool overflow = ((left ^ right) & (left ^ result) &
		(std::uint64_t{1} << 63)) != 0;
	return ((result >> 63) != 0 ? 0x8000'0000U : 0) |
		(result == 0 ? 0x4000'0000U : 0) |
		(left >= right ? 0x2000'0000U : 0) |
		(overflow ? 0x1000'0000U : 0);
}

[[nodiscard]] arm64::CpuState initial_state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index) {
		cpu.registers[index] =
			(static_cast<std::uint64_t>(index & 15U) << 56) |
			(0x0000'1357'9bdf'0246ULL ^
			 static_cast<std::uint64_t>(index) * 0x0001'0101'0101'0101ULL);
	}
	cpu.sp = 0xaf80'0000'0000'0040ULL;
	cpu.nzcv = 0xd000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.exclusive_address = 0x2000;
	cpu.exclusive_key = 0x3000;
	cpu.exclusive_value = 0x4000;
	cpu.exclusive_value2 = 0x5000;
	cpu.exclusive_version = 0x6000;
	cpu.exclusive_size = 8;
	return cpu;
}

void require_state(const arm64::CpuState &actual,
	const arm64::CpuState &expected, std::uint64_t expected_pc)
{
	require(actual.registers == expected.registers);
	require(actual.sp == expected.sp && actual.pc == expected_pc);
	for (unsigned index = 0; index < actual.vectors.size(); ++index)
		require(actual.vectors[index].bytes == expected.vectors[index].bytes);
	require(actual.nzcv == expected.nzcv && actual.fpcr == expected.fpcr &&
		actual.fpsr == expected.fpsr && actual.tls == expected.tls);
	require(actual.mte_enabled == expected.mte_enabled &&
		actual.mte_exclude == expected.mte_exclude &&
		actual.mte_tag_choices == expected.mte_tag_choices &&
		actual.mte_random_tags == expected.mte_random_tags &&
		actual.mte_seed == expected.mte_seed &&
		actual.mte_tag == expected.mte_tag &&
		actual.mte_random_state == expected.mte_random_state);
	require(actual.exclusive_address == expected.exclusive_address &&
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
}

class Harness final {
public:
	Harness() : tlb_{std::make_unique<gadgets::Tlb>()}
	{
		tlb_->prepare(0, darwin_uml_static_vm_changes(0));
	}

	void check(std::uint32_t instruction, const arm64::CpuState &initial,
		const arm64::CpuState &expected)
	{
		auto interpreted = initial;
		interpreted.pc = pc;
		const auto result = arm64::Dispatcher{}.execute(
			interpreted, memory_, instruction);
		require(result.reason == arm64::StopReason::continued &&
			result.instructions == 1 && result.address == pc &&
			result.instruction == instruction && !result.write);
		require_state(interpreted, expected, pc + 4);

		const auto built = gadgets::Builder{}.build(pc,
			std::array{instruction, 0xd400'0001U});
		require(built.complete && built.program.instruction_count() == 2);
		auto gadget = initial;
		gadget.pc = pc;
		require(gadgets::execute(built.program, gadget, *tlb_) ==
			gadgets::Event::system_call);
		require_state(gadget, expected, pc + 8);
		require(memory_.reads == 0 && memory_.writes == 0);
	}

private:
	RejectingMemory memory_;
	std::unique_ptr<gadgets::Tlb> tlb_;
};

void encoding_examples()
{
	require(gmi(0, 1, 2) == 0x9ac2'1420U);
	require(gmi(30, 31, 31) == 0x9adf'17feU);
	require(subp(false, 0, 1, 2) == 0x9ac2'0020U);
	require(subp(false, 30, 31, 31) == 0x9adf'03feU);
	require(subp(true, 0, 1, 2) == 0xbac2'0020U);
	require(subp(true, 31, 1, 2) == 0xbac2'003fU); // CMPP
	require(subp(true, 31, 31, 31) == 0xbadf'03ffU); // CMPP SP, SP
}

void exhaustive_register_encodings(Harness &harness)
{
	for (unsigned destination = 0; destination < 32; ++destination) {
		for (unsigned first_register = 0; first_register < 32;
		     ++first_register) {
			for (unsigned second_register = 0; second_register < 32;
			     ++second_register) {
				const auto initial = initial_state();
				const auto first = first_register == 31 ? initial.sp :
					initial.registers[first_register];
				const auto second_gmi = second_register == 31 ? 0 :
					initial.registers[second_register];
				auto expected = initial;
				if (destination != 31)
					expected.registers[destination] = second_gmi |
						(std::uint64_t{1} << ((first >> 56) & 15U));
				harness.check(gmi(destination, first_register,
					second_register), initial, expected);

				const auto second = second_register == 31 ? initial.sp :
					initial.registers[second_register];
				const auto left = sign_extend_56(first);
				const auto right = sign_extend_56(second);
				const auto difference = left - right;
				for (const bool flags : {false, true}) {
					expected = initial;
					if (destination != 31)
						expected.registers[destination] = difference;
					if (flags)
						expected.nzcv = subtract_flags(left, right);
					harness.check(subp(flags, destination,
						first_register, second_register), initial, expected);
				}
			}
		}
	}
}

void gmi_tag_and_mask_edges(Harness &harness)
{
	constexpr std::array masks{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0xffff},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL}};
	for (unsigned tag = 0; tag < 16; ++tag) {
		for (const auto mask : masks) {
			auto initial = initial_state();
			initial.registers[1] =
				0xf000'1234'5678'9abcULL |
				(static_cast<std::uint64_t>(tag) << 56);
			initial.registers[2] = mask;
			auto expected = initial;
			expected.registers[3] = mask | (std::uint64_t{1} << tag);
			harness.check(gmi(3, 1, 2), initial, expected);
		}
	}

	/* SP is an address source, while register 31 in the mask position is XZR. */
	auto initial = initial_state();
	initial.sp = 0xfa00'0000'0000'0000ULL;
	auto expected = initial;
	expected.registers[4] = std::uint64_t{1} << 10;
	harness.check(gmi(4, 31, 31), initial, expected);
}

void pointer_arithmetic_edges(Harness &harness)
{
	constexpr std::array values{
		std::uint64_t{0}, std::uint64_t{1},
		std::uint64_t{0x007f'ffff'ffff'ffffULL},
		std::uint64_t{0x0080'0000'0000'0000ULL},
		std::uint64_t{0x00ff'ffff'ffff'ffffULL},
		std::uint64_t{0xab00'0000'0000'0001ULL},
		std::uint64_t{0xcd7f'ffff'ffff'ffffULL},
		std::uint64_t{0xef80'0000'0000'0000ULL}};
	for (const auto first : values) {
		for (const auto second : values) {
			for (const bool flags : {false, true}) {
				auto initial = initial_state();
				initial.registers[1] = first;
				initial.registers[2] = second;
				const auto left = sign_extend_56(first);
				const auto right = sign_extend_56(second);
				auto expected = initial;
				expected.registers[3] = left - right;
				if (flags)
					expected.nzcv = subtract_flags(left, right);
				harness.check(subp(flags, 3, 1, 2), initial, expected);
			}
		}
	}
}

void production_specialization()
{
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	return;
#else
	std::set<std::uintptr_t> entries;
	for (unsigned operation = 0; operation < 3; ++operation) {
		for (unsigned mode = 0; mode < 8; ++mode) {
			const unsigned first = (mode & 4U) != 0 ? 31U : 1U;
			const unsigned second = (mode & 2U) != 0 ? 31U : 2U;
			const unsigned destination = (mode & 1U) != 0 ? 31U : 3U;
			const auto instruction = operation == 0 ?
				gmi(destination, first, second) :
				subp(operation == 2, destination, first, second);
			const auto built = gadgets::Builder{}.build(pc,
				std::array{instruction});
			require(built.complete && built.program.instruction_count() == 1 &&
				built.program.words().size() == 4);
			entries.insert(built.program.words().front());
		}
	}
	/* Three operations times all eight translation-time SP/ZR modes. */
	require(entries.size() == 24);
#endif
}

} // namespace

int main()
{
	encoding_examples();
	Harness harness;
	exhaustive_register_encodings(harness);
	gmi_tag_and_mask_edges(harness);
	pointer_arithmetic_edges(harness);
	production_specialization();
}
