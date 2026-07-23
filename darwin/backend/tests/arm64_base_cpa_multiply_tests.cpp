// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <set>
#include <source_location>

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x5f0'0000;

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

[[nodiscard]] constexpr std::uint32_t multiply_instruction(bool subtract,
	unsigned destination, unsigned left, unsigned right, unsigned addend)
{
	return (subtract ? 0x9b60'8000U : 0x9b60'0000U) |
		((right & 31U) << 16) | ((addend & 31U) << 10) |
		((left & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint64_t pointer_check(bool add_enabled,
	bool multiply_enabled, std::uint64_t result, std::uint64_t base,
	bool multiply_overflow)
{
	const bool previous = ((base >> 55) & 1U) != ((base >> 54) & 1U);
	const bool addition_detected = (result >> 56) != (base >> 56) || previous;
	if (!(add_enabled && addition_detected) &&
	    !(multiply_enabled && multiply_overflow))
		return result;
	constexpr auto low_mask = (std::uint64_t{1} << 54) - 1;
	return (result & low_mask) | (base & 0xff80'0000'0000'0000ULL) |
		(((~base >> 55) & 1U) << 54);
}

struct ArithmeticResult final {
	std::uint64_t value{};
	bool overflow{};
};

[[nodiscard]] ArithmeticResult arithmetic(bool subtract, std::uint64_t left,
	std::uint64_t right, std::uint64_t addend)
{
	const auto product = static_cast<__int128>(
		std::bit_cast<std::int64_t>(left)) * static_cast<__int128>(
		std::bit_cast<std::int64_t>(right));
	const bool overflow = product < std::numeric_limits<std::int64_t>::min() ||
		product > std::numeric_limits<std::int64_t>::max();
	const auto low = static_cast<std::uint64_t>(product);
	return {subtract ? addend - low : addend + low, overflow};
}

[[nodiscard]] arm64::CpuState state(bool add_enabled = true,
	bool multiply_enabled = true)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			(static_cast<std::uint64_t>(0x10U + index * 11U) << 56) |
			(0x0042'1000'1234'0001ULL +
			 static_cast<std::uint64_t>(index) * 0x10101ULL);
	cpu.sp = 0xcd80'0000'0000'0200ULL;
	cpu.nzcv = 0xb000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.set_mte_configuration(true, 0x8421U);
	cpu.set_mte_generator(true, 0xace1U, 7, 0xfeed'face'0123'4567ULL);
	cpu.cpa_enabled = add_enabled;
	cpu.cpa_multiply_enabled = multiply_enabled;
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
	const arm64::CpuState &expected, std::uint64_t expected_pc)
{
	require(actual.registers == expected.registers && actual.sp == expected.sp &&
		actual.pc == expected_pc && actual.nzcv == expected.nzcv &&
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

[[nodiscard]] arm64::CpuState expected_result(const arm64::CpuState &initial,
	bool subtract, unsigned destination, unsigned left_register,
	unsigned right_register, unsigned addend_register)
{
	auto expected = initial;
	const auto left = left_register == 31 ? 0 :
		initial.registers[left_register];
	const auto right = right_register == 31 ? 0 :
		initial.registers[right_register];
	const auto addend = addend_register == 31 ? 0 :
		initial.registers[addend_register];
	const auto calculated = arithmetic(subtract, left, right, addend);
	const auto result = pointer_check(initial.cpa_enabled,
		initial.cpa_multiply_enabled, calculated.value, addend,
		calculated.overflow);
	if (destination != 31)
		expected.registers[destination] = result;
	return expected;
}

class Harness final {
public:
	Harness() : tlb_{std::make_unique<gadgets::Tlb>()}
	{
		tlb_->prepare(0, darwin_uml_static_vm_changes(0));
	}

	void check(bool subtract, unsigned destination, unsigned left,
		unsigned right, unsigned addend, const arm64::CpuState &initial)
	{
		const auto instruction = multiply_instruction(
			subtract, destination, left, right, addend);
		const auto expected = expected_result(
			initial, subtract, destination, left, right, addend);
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
	require(multiply_instruction(false, 0, 1, 2, 3) == 0x9b62'0c20U);
	require(multiply_instruction(false, 30, 31, 31, 31) == 0x9b7f'7ffeU);
	require(multiply_instruction(true, 0, 1, 2, 3) == 0x9b62'8c20U);
	require(multiply_instruction(true, 30, 31, 31, 31) == 0x9b7f'fffeU);
}

void exhaustive_encodings(Harness &harness)
{
	const auto initial = state();
	for (const bool subtract : {false, true})
		for (unsigned destination = 0; destination < 32; ++destination)
			for (unsigned left = 0; left < 32; ++left)
				for (unsigned right = 0; right < 32; ++right)
					for (unsigned addend = 0; addend < 32; ++addend)
						harness.check(subtract, destination, left,
							right, addend, initial);
}

void overflow_and_control_edges(Harness &harness)
{
	constexpr std::array values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2},
		std::uint64_t{0x3fff'ffff'ffff'ffffULL},
		std::uint64_t{0x4000'0000'0000'0000ULL},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL},
		std::uint64_t{0xffff'ffff'ffff'fffeULL}};
	constexpr std::array addends{
		std::uint64_t{0}, std::uint64_t{0x003f'ffff'ffff'ffffULL},
		std::uint64_t{0x0040'0000'0000'0000ULL},
		std::uint64_t{0x0080'0000'0000'0000ULL},
		std::uint64_t{0x7f00'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL}};
	for (const bool add_enabled : {false, true})
		for (const bool multiply_enabled : {false, true})
			for (const auto left : values)
				for (const auto right : values)
					for (const auto addend : addends)
						for (const bool subtract : {false, true}) {
							auto initial = state(
								add_enabled, multiply_enabled);
							initial.registers[1] = left;
							initial.registers[2] = right;
							initial.registers[3] = addend;
							harness.check(
								subtract, 4, 1, 2, 3, initial);
						}
}

void production_specialization()
{
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	return;
#else
	std::set<std::uintptr_t> entries;
	for (const bool subtract : {false, true})
		for (const bool left_zero : {false, true})
			for (const bool right_zero : {false, true})
				for (const bool addend_zero : {false, true})
					for (const bool destination_zero : {false, true}) {
						const auto built = gadgets::Builder{}.build(pc,
							std::array{multiply_instruction(subtract,
								destination_zero ? 31U : 4U,
								left_zero ? 31U : 1U,
								right_zero ? 31U : 2U,
								addend_zero ? 31U : 3U)});
							if (!(built.complete &&
							      built.program.instruction_count() == 1 &&
							      built.program.words().size() == 4)) {
								std::fprintf(stderr,
									"bad specialization sub=%u n=%u m=%u a=%u d=%u complete=%u instructions=%zu words=%zu\n",
									subtract, left_zero, right_zero,
									addend_zero, destination_zero,
									built.complete,
									built.program.instruction_count(),
									built.program.words().size());
								std::abort();
							}
						entries.insert(built.program.words().front());
					}
	require(entries.size() == 32);
#endif
}

} // namespace

int main()
{
	encoding_examples();
	Harness harness;
	exhaustive_encodings(harness);
	overflow_and_control_edges(harness);
	production_specialization();
}
