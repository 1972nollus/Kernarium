// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <source_location>

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x5d0'0000;

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

[[nodiscard]] constexpr std::uint32_t irg(
	unsigned destination, unsigned source, unsigned exclude)
{
	return 0x9ac0'1000U | ((exclude & 31U) << 16) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] std::uint8_t select_allowed(
	std::uint16_t allowed, unsigned selection)
{
	for (unsigned tag = 0; tag < 16; ++tag) {
		if ((allowed & (1U << tag)) == 0)
			continue;
		if (selection-- == 0)
			return static_cast<std::uint8_t>(tag);
	}
	std::abort();
}

[[nodiscard]] std::uint8_t reference_tag(
	arm64::CpuState &cpu, std::uint16_t exclude)
{
	if (!cpu.mte_enabled || exclude == 0xffffU)
		return 0;
	const auto allowed = static_cast<std::uint16_t>(~exclude);
	const unsigned count = std::popcount(allowed);
	unsigned selection;
	if (cpu.mte_random_tags) {
		auto random = cpu.mte_random_state;
		random ^= random << 13;
		random ^= random >> 7;
		random ^= random << 17;
		cpu.mte_random_state = random;
		selection = static_cast<unsigned>(random % count);
	} else {
		unsigned generator_tag = static_cast<unsigned>(cpu.mte_tag) + 1U;
		if (generator_tag >= count)
			generator_tag = 0;
		cpu.mte_tag = static_cast<std::uint8_t>(generator_tag);
		selection = static_cast<unsigned>(
			(generator_tag + count * static_cast<unsigned>(cpu.mte_seed)) >> 16) %
			count;
		const unsigned feedback = ((cpu.mte_seed >> 5) ^
			(cpu.mte_seed >> 3) ^ (cpu.mte_seed >> 2) ^ cpu.mte_seed) & 1U;
		cpu.mte_seed = static_cast<std::uint16_t>(
			(cpu.mte_seed >> 1) | (feedback << 15));
	}
	return select_allowed(allowed, selection);
}

[[nodiscard]] arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			(static_cast<std::uint64_t>(index & 15U) << 56) |
			(0xc000'1234'5678'9000ULL +
			 static_cast<std::uint64_t>(index) * 0x101ULL);
	cpu.sp = 0xdf00'ffff'ffff'fff0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.set_mte_configuration(true, 0x8001U);
	cpu.set_mte_generator(false, 0xace1U, 7);
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
	unsigned destination, unsigned source_register, unsigned exclude_register)
{
	auto expected = initial;
	const auto source = source_register == 31 ? initial.sp :
		initial.registers[source_register];
	const auto register_exclude = exclude_register == 31 ? 0 :
		initial.registers[exclude_register];
	const auto exclude = static_cast<std::uint16_t>(
		initial.mte_exclude | register_exclude);
	const auto tag = reference_tag(expected, exclude);
	const auto result = (source & ~0x0f00'0000'0000'0000ULL) |
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

	void check(unsigned destination, unsigned source, unsigned exclude,
		const arm64::CpuState &initial)
	{
		const auto instruction = irg(destination, source, exclude);
		const auto expected = expected_result(
			initial, destination, source, exclude);
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
	require(irg(0, 1, 2) == 0x9ac2'1020U);
	require(irg(31, 31, 31) == 0x9adf'13ffU);
}

void exhaustive_register_encodings(Harness &harness)
{
	for (unsigned destination = 0; destination < 32; ++destination)
		for (unsigned source = 0; source < 32; ++source)
			for (unsigned exclude = 0; exclude < 32; ++exclude) {
				auto initial = state();
				initial.mte_seed = static_cast<std::uint16_t>(
					0x1001U + destination * 257U + source * 17U + exclude);
				initial.mte_tag = static_cast<std::uint8_t>(
					(destination + source + exclude) & 15U);
				harness.check(destination, source, exclude, initial);
			}
}

void every_exclusion_mask(Harness &harness)
{
	for (unsigned raw = 0; raw <= 0xffffU; ++raw) {
		auto initial = state();
		initial.set_mte_configuration(true, static_cast<std::uint16_t>(raw));
		initial.set_mte_generator(false,
			static_cast<std::uint16_t>(raw ^ 0xa5a5U),
			static_cast<std::uint8_t>(raw & 15U));
		harness.check(2, 1, 31, initial);
	}
}

void deterministic_generator_sequences(Harness &harness)
{
	constexpr std::array excludes{
		std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0x8000},
		std::uint16_t{0xaaaa}, std::uint16_t{0x5555},
		std::uint16_t{0xfffe}, std::uint16_t{0x7fff}};
	for (const auto exclude : excludes)
		for (const std::uint16_t seed : {std::uint16_t{0},
		     std::uint16_t{1}, std::uint16_t{0xace1},
		     std::uint16_t{0xffff}})
			for (unsigned tag = 0; tag < 16; ++tag) {
				auto current = state();
				current.set_mte_configuration(true, exclude);
				current.set_mte_generator(false, seed,
					static_cast<std::uint8_t>(tag));
				for (unsigned iteration = 0; iteration < 20; ++iteration) {
					harness.check(2, 1, 31, current);
					current = expected_result(current, 2, 1, 31);
				}
			}
}

void random_mode_and_zero_cases(Harness &harness)
{
	constexpr std::array states{
		std::uint64_t{0}, std::uint64_t{1},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL}};
	for (const auto random_state : states) {
		for (const std::uint16_t exclude : {std::uint16_t{0},
		     std::uint16_t{0xaaaa}, std::uint16_t{0xfffe},
		     std::uint16_t{0x7fff}}) {
			auto current = state();
			current.set_mte_configuration(true, exclude);
			current.set_mte_generator(true, 0x1234U, 9, random_state);
			for (unsigned iteration = 0; iteration < 20; ++iteration) {
				harness.check(2, 1, 31, current);
				current = expected_result(current, 2, 1, 31);
			}
		}
	}

	for (const bool enabled : {false, true}) {
		auto initial = state();
		initial.set_mte_configuration(enabled, 0xffffU);
		initial.set_mte_generator(false, 0xace1U, 7);
		const auto before_seed = initial.mte_seed;
		const auto before_tag = initial.mte_tag;
		harness.check(31, 31, 31, initial);
		const auto expected = expected_result(initial, 31, 31, 31);
		require(expected.mte_seed == before_seed && expected.mte_tag == before_tag);
	}
}

void combined_register_and_gcr_excludes(Harness &harness)
{
	for (unsigned gcr = 0; gcr < 16; ++gcr)
		for (unsigned reg = 0; reg < 16; ++reg) {
			auto initial = state();
			initial.set_mte_configuration(true,
				static_cast<std::uint16_t>(1U << gcr));
			initial.registers[2] = std::uint64_t{1} << reg;
			harness.check(3, 1, 2, initial);
		}
}

void production_specialization()
{
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	return;
#else
	std::set<std::uintptr_t> entries;
	for (const bool source_sp : {false, true})
		for (const bool mask_zero : {false, true})
			for (const bool destination_sp : {false, true}) {
				const auto built = gadgets::Builder{}.build(pc,
					std::array{irg(destination_sp ? 31U : 3U,
						source_sp ? 31U : 1U,
						mask_zero ? 31U : 2U)});
				require(built.complete &&
					built.program.instruction_count() == 1 &&
					built.program.words().size() == 5);
				entries.insert(built.program.words().front());
			}
	require(entries.size() == 8);
#endif
}

} // namespace

int main()
{
	encoding_examples();
	Harness harness;
	exhaustive_register_encodings(harness);
	every_exclusion_mask(harness);
	deterministic_generator_sequences(harness);
	random_mode_and_zero_cases(harness);
	combined_register_and_gcr_excludes(harness);
	production_specialization();
}
