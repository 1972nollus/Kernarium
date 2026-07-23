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

constexpr std::uint64_t pc = 0x5c0'0000;

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

[[nodiscard]] constexpr std::uint32_t add_sub_tag(bool subtract,
	unsigned destination, unsigned source, unsigned immediate,
	unsigned tag_offset)
{
	return (subtract ? 0xd180'0000U : 0x9180'0000U) |
		((immediate & 63U) << 16) | ((tag_offset & 15U) << 10) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] std::uint8_t reference_tag(bool enabled, std::uint16_t exclude,
	unsigned start, unsigned offset)
{
	if (!enabled || exclude == 0xffffU)
		return 0;
	unsigned tag = start & 15U;
	unsigned remaining = offset & 15U;
	if (remaining == 0) {
		do {
			if ((exclude & (1U << tag)) == 0)
				return static_cast<std::uint8_t>(tag);
			tag = (tag + 1U) & 15U;
		} while (tag != (start & 15U));
	} else {
		while (remaining != 0) {
			tag = (tag + 1U) & 15U;
			if ((exclude & (1U << tag)) == 0)
				--remaining;
		}
		return static_cast<std::uint8_t>(tag);
	}
	return 0;
}

[[nodiscard]] arm64::CpuState state(bool enabled = true,
	std::uint16_t exclude = 0)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			(static_cast<std::uint64_t>(index & 15U) << 56) |
			(0xa000'0123'4567'89a0ULL +
			 static_cast<std::uint64_t>(index) * 0x1010ULL);
	cpu.sp = 0xbf00'ffff'ffff'fff0ULL;
	cpu.nzcv = 0xb000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.set_mte_configuration(enabled, exclude);
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
		actual.mte_random_state == expected.mte_random_state);
	for (unsigned index = 0; index < actual.vectors.size(); ++index)
		require(actual.vectors[index].bytes == expected.vectors[index].bytes);
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

[[nodiscard]] arm64::CpuState expected_result(const arm64::CpuState &initial,
	bool subtract, unsigned destination, unsigned source_register,
	unsigned immediate, unsigned tag_offset)
{
	auto expected = initial;
	const auto source = source_register == 31 ? initial.sp :
		initial.registers[source_register];
	const auto offset = static_cast<std::uint64_t>(immediate & 63U) << 4;
	auto result = subtract ? source - offset : source + offset;
	const auto tag = reference_tag(initial.mte_enabled, initial.mte_exclude,
		static_cast<unsigned>(source >> 56), tag_offset);
	result = (result & ~0x0f00'0000'0000'0000ULL) |
		(static_cast<std::uint64_t>(tag) << 56);
	if (destination == 31)
		expected.sp = result;
	else
		expected.registers[destination] = result;
	return expected;
}

class Harness final {
public:
	Harness() : tlb_{std::make_unique<gadgets::Tlb>()}
	{
		tlb_->prepare(0, darwin_uml_static_vm_changes(0));
	}

	void check(bool subtract, unsigned destination, unsigned source,
		unsigned immediate, unsigned tag_offset,
		const arm64::CpuState &initial)
	{
		const auto instruction = add_sub_tag(subtract, destination, source,
			immediate, tag_offset);
		const auto expected = expected_result(initial, subtract, destination,
			source, immediate, tag_offset);
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
	require(add_sub_tag(false, 0, 1, 0, 0) == 0x9180'0020U);
	require(add_sub_tag(false, 30, 31, 63, 15) == 0x91bf'3ffeU);
	require(add_sub_tag(false, 31, 31, 1, 1) == 0x9181'07ffU);
	require(add_sub_tag(true, 0, 1, 0, 0) == 0xd180'0020U);
	require(add_sub_tag(true, 30, 31, 63, 15) == 0xd1bf'3ffeU);
	require(add_sub_tag(true, 31, 31, 1, 1) == 0xd181'07ffU);
}

void derived_table_matches_reference()
{
	for (const bool enabled : {false, true}) {
		for (const std::uint16_t exclude : {std::uint16_t{0},
		     std::uint16_t{1}, std::uint16_t{0x8000},
		     std::uint16_t{0xaaaa}, std::uint16_t{0x5555},
		     std::uint16_t{0xfffe}, std::uint16_t{0x7fff},
		     std::uint16_t{0xffff}}) {
			auto cpu = state(enabled, exclude);
			for (unsigned start = 0; start < 16; ++start)
				for (unsigned offset = 0; offset < 16; ++offset)
					require(cpu.mte_tag_choices[start * 16 + offset] ==
						reference_tag(enabled, exclude, start, offset));
		}
	}
}

void exhaustive_register_encodings(Harness &harness)
{
	const auto initial = state(true, 0x8421U);
	for (const bool subtract : {false, true})
		for (unsigned destination = 0; destination < 32; ++destination)
			for (unsigned source = 0; source < 32; ++source)
				harness.check(subtract, destination, source,
					(source * 5U + destination) & 63U,
					(source + destination * 3U) & 15U, initial);
}

void exhaustive_immediates_tags_and_edges(Harness &harness)
{
	constexpr std::array excludes{
		std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0x8000},
		std::uint16_t{0xaaaa}, std::uint16_t{0x5555},
		std::uint16_t{0xfffe}, std::uint16_t{0x7fff},
		std::uint16_t{0xffff}};
	for (const auto exclude : excludes) {
		for (unsigned start = 0; start < 16; ++start) {
			auto initial = state(true, exclude);
			initial.registers[1] = 0xa000'ffff'ffff'fc10ULL |
				(static_cast<std::uint64_t>(start) << 56);
			for (unsigned immediate = 0; immediate < 64; ++immediate)
				for (unsigned tag_offset = 0; tag_offset < 16; ++tag_offset)
					for (const bool subtract : {false, true})
						harness.check(subtract, 2, 1, immediate,
							tag_offset, initial);
		}
	}
}

void every_exclusion_mask_and_disabled_behavior(Harness &harness)
{
	for (unsigned raw_exclude = 0; raw_exclude <= 0xffffU; ++raw_exclude) {
		const auto exclude = static_cast<std::uint16_t>(raw_exclude);
		auto initial = state(true, exclude);
		const unsigned start = raw_exclude & 15U;
		const unsigned tag_offset = (raw_exclude >> 4) & 15U;
		initial.registers[1] = 0xe000'0000'0000'0010ULL |
			(static_cast<std::uint64_t>(start) << 56);
		harness.check(false, 2, 1, raw_exclude & 63U, tag_offset, initial);
		harness.check(true, 2, 1, raw_exclude & 63U, tag_offset, initial);
	}

	for (unsigned start = 0; start < 16; ++start) {
		auto initial = state(false, 0x8421U);
		initial.sp = 0xf000'0000'0000'0000ULL |
			(static_cast<std::uint64_t>(start) << 56);
		for (unsigned tag_offset = 0; tag_offset < 16; ++tag_offset)
			for (const bool subtract : {false, true})
				harness.check(subtract, 31, 31, 63, tag_offset, initial);
	}
}

void production_specialization()
{
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	return;
#else
	std::set<std::uintptr_t> entries;
	for (const bool subtract : {false, true})
		for (const bool source_sp : {false, true})
			for (const bool destination_sp : {false, true}) {
				const auto instruction = add_sub_tag(subtract,
					destination_sp ? 31U : 2U,
					source_sp ? 31U : 1U, 63, 15);
				const auto built = gadgets::Builder{}.build(pc,
					std::array{instruction});
				require(built.complete &&
					built.program.instruction_count() == 1 &&
					built.program.words().size() == 4);
				entries.insert(built.program.words().front());
			}
	require(entries.size() == 8);
#endif
}

} // namespace

int main()
{
	encoding_examples();
	derived_table_matches_reference();
	Harness harness;
	exhaustive_register_encodings(harness);
	exhaustive_immediates_tags_and_edges(harness);
	every_exclusion_mask_and_disabled_behavior(harness);
	production_specialization();
}
