// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_mte_test_support.hpp"

#include <array>
#include <cstdint>
#include <set>

namespace mte = arm64_base_mte_test;
namespace test = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

void encoding_examples()
{
	mte::require(mte::irg(0, 1, 2) == 0x9ac2'1020U);
	mte::require(mte::irg(31, 31, 31) == 0x9adf'13ffU);
}

void validate_random_result(
	const mte::arm64::CpuState &initial,
	const mte::arm64::CpuState &expected_template,
	const mte::arm64::CpuState &actual,
	std::string_view case_name, std::string_view engine,
	std::uint32_t instruction)
{
	const unsigned destination = instruction & 31U;
	const unsigned source = (instruction >> 5) & 31U;
	const unsigned exclude_register = (instruction >> 16) & 31U;
	const auto source_value = mte::read_register(initial, source, true);
	const auto result = mte::read_register(actual, destination, true);
	mte::require((result & ~mte::logical_tag_mask) ==
		(source_value & ~mte::logical_tag_mask));

	const auto register_exclude =
		mte::read_register(initial, exclude_register, false);
	const auto exclude = static_cast<std::uint16_t>(
		initial.mte_exclude | register_exclude);
	const auto tag = mte::logical_tag(result);
	if (!initial.mte_enabled || exclude == 0xffffU)
		mte::require(tag == 0);
	else
		mte::require((exclude & (1U << tag)) == 0);

	/*
	 * The selected tag and private random-generator state are
	 * implementation-defined. Preserve every other architectural field and
	 * compare them through the common harness.
	 */
	auto expected = initial;
	expected.pc = expected_template.pc;
	mte::write_register(expected, destination, result, true);
	expected.mte_random_state = actual.mte_random_state;
	test::require_architecture_equal(
		expected, actual, case_name, engine, instruction);
}

void all_register_fields(test::EngineHarness &harness)
{
	for (unsigned destination = 0; destination < 32; ++destination) {
		for (unsigned source = 0; source < 32; ++source) {
			for (unsigned exclude = 0; exclude < 32; ++exclude) {
				auto initial = mte::initial_state();
				initial.mte_seed = static_cast<std::uint16_t>(
					0x1001U + destination * 257U +
					source * 17U + exclude);
				initial.mte_tag = static_cast<std::uint8_t>(
					(destination + source + exclude) & 15U);
				const mte::CaseName name{"irg-d%u-n%u-m%u",
					destination, source, exclude};
				mte::check_one(harness, name,
					mte::irg(destination, source, exclude),
					initial, mte::expected_irg(
						initial, destination, source, exclude));
			}
		}
	}
}

void every_exclusion_mask(test::EngineHarness &harness)
{
	for (unsigned raw = 0; raw <= 0xffffU; ++raw) {
		auto initial = mte::initial_state();
		initial.set_mte_configuration(
			true, static_cast<std::uint16_t>(raw));
		initial.set_mte_generator(false,
			static_cast<std::uint16_t>(raw ^ 0xa5a5U),
			static_cast<std::uint8_t>(raw & 15U));
		const mte::CaseName name{"irg-exclude-%04x", raw};
		mte::check_one(harness, name, mte::irg(2, 1, 31), initial,
			mte::expected_irg(initial, 2, 1, 31));
	}
}

void check_sequence(test::EngineHarness &harness, std::string_view name,
	const arm64_base_mte_test::arm64::CpuState &initial,
	unsigned destination, unsigned source, unsigned exclude_register,
	unsigned repetitions)
{
	std::array<test::ProgramWord, 33> words{};
	mte::require(repetitions < words.size());
	const auto instruction =
		mte::irg(destination, source, exclude_register);
	for (unsigned index = 0; index < repetitions; ++index)
		words[index] = {
			.offset = index * sizeof(std::uint32_t),
			.instruction = instruction,
		};
	words[repetitions] = {
		.offset = repetitions * sizeof(std::uint32_t),
		.instruction = 0xd400'0001U,
	};
	auto expected = initial;
	for (unsigned index = 0; index < repetitions; ++index)
		expected = mte::expected_irg(
			expected, destination, source, exclude_register);
	expected.pc = test::engine_test_code_base +
		(repetitions + 1) * sizeof(std::uint32_t);
	harness.check_program({
		.name = name,
		.code = std::span<const test::ProgramWord>{
			words.data(), repetitions + 1},
		.initial = initial,
		.expected = expected,
		.native = mte::native_mte,
	});
}

void deterministic_sequences(test::EngineHarness &harness)
{
	constexpr std::array excludes{
		std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0x8000},
		std::uint16_t{0xaaaa}, std::uint16_t{0x5555},
		std::uint16_t{0xfffe}, std::uint16_t{0x7fff}};
	for (const auto exclude : excludes) {
		for (const std::uint16_t seed : {std::uint16_t{0},
		     std::uint16_t{1}, std::uint16_t{0xace1},
		     std::uint16_t{0xffff}}) {
			for (unsigned tag = 0; tag < 16; ++tag) {
				auto initial = mte::initial_state();
				initial.set_mte_configuration(true, exclude);
				initial.set_mte_generator(
					false, seed, static_cast<std::uint8_t>(tag));
				const mte::CaseName name{
					"irg-sequence-deterministic-e%x-s%x-t%u",
					static_cast<unsigned>(exclude),
					static_cast<unsigned>(seed), tag};
				check_sequence(harness, name, initial, 2, 1, 31, 20);
			}
		}
	}

}

void random_architectural_invariants(test::EngineHarness &harness)
{
	constexpr std::array excludes{
		std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0x8000},
		std::uint16_t{0xaaaa}, std::uint16_t{0x5555},
		std::uint16_t{0xfffe}, std::uint16_t{0x7fff}};
	constexpr std::array random_states{
		std::uint64_t{0}, std::uint64_t{1},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL}};
	struct RegisterCase final {
		unsigned destination;
		unsigned source;
	};
	constexpr std::array register_cases{
		RegisterCase{2, 1},
		RegisterCase{1, 1},
		RegisterCase{31, 1},
		RegisterCase{2, 31},
		RegisterCase{31, 31},
	};
	for (const auto random_state : random_states) {
		for (const auto exclude : excludes) {
			for (const auto registers : register_cases) {
				auto initial = mte::initial_state();
				initial.set_mte_configuration(
					true, exclude & 0xaaaaU);
				initial.registers[30] = exclude & 0x5555U;
				initial.set_mte_generator(
					true, 0x1234U, 9, random_state);
				const mte::CaseName name{
					"irg-random-r%llx-e%x-d%u-n%u",
					static_cast<unsigned long long>(random_state),
					static_cast<unsigned>(exclude),
					registers.destination, registers.source};
				harness.check({
					.name = name,
					.instruction = mte::irg(
						registers.destination,
						registers.source, 30),
					.initial = initial,
					.expected = initial,
					.native = mte::native_mte,
					.validator = &validate_random_result,
				});
			}
		}
	}
}

void combined_and_zero_excludes(test::EngineHarness &harness)
{
	for (unsigned global = 0; global < 16; ++global) {
		for (unsigned local = 0; local < 16; ++local) {
			auto initial = mte::initial_state();
			initial.set_mte_configuration(true,
				static_cast<std::uint16_t>(1U << global));
			initial.registers[2] = std::uint64_t{1} << local;
			const mte::CaseName name{
				"irg-combined-global%u-local%u", global, local};
			mte::check_one(harness, name, mte::irg(3, 1, 2),
				initial, mte::expected_irg(initial, 3, 1, 2));
		}
	}

	for (const bool enabled : {false, true}) {
		for (const bool random : {false, true}) {
			auto initial = mte::initial_state();
			initial.set_mte_configuration(enabled, 0xffffU);
			initial.set_mte_generator(
				random, 0xace1U, 7, 0x1234'5678ULL);
			const mte::CaseName name{"irg-zero-e%u-r%u",
				static_cast<unsigned>(enabled),
				static_cast<unsigned>(random)};
			mte::check_one(harness, name, mte::irg(31, 31, 31),
				initial, mte::expected_irg(initial, 31, 31, 31));
		}
	}
}

void reserved_allocations(test::EngineHarness &harness)
{
	const auto initial = mte::initial_state();
	constexpr std::array invalid{
		0x1ac0'1020U,
		0xbac0'1020U,
		0x9ac0'1820U,
	};
	for (unsigned index = 0; index < invalid.size(); ++index) {
		const mte::CaseName name{"irg-reserved-%u", index};
		harness.check_illegal(name, invalid[index], initial, mte::native_mte);
	}
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool source_sp : {false, true}) {
		for (const bool mask_zero : {false, true}) {
			for (const bool destination_sp : {false, true}) {
				const auto built = gadgets::Builder{}.build(
					test::engine_test_code_base,
					std::array{mte::irg(
						destination_sp ? 31U : 3U,
						source_sp ? 31U : 1U,
						mask_zero ? 31U : 2U)});
				mte::require(built.complete &&
					built.program.instruction_count() == 1 &&
					built.program.words().size() == 5);
				entries.insert(built.program.words().front());
			}
		}
	}
	mte::require(entries.size() == 8);
}

} // namespace

int main()
{
	encoding_examples();
	test::EngineHarness harness;
	all_register_fields(harness);
	every_exclusion_mask(harness);
	deterministic_sequences(harness);
	random_architectural_invariants(harness);
	combined_and_zero_excludes(harness);
	reserved_allocations(harness);
	production_specialization();
	harness.print_summary("arm64-base-mte-irg");
}
