// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_multiply_test_support.hpp"

#include <array>
#include <cstdio>

namespace uml::backend::tests::base_multiply {
namespace {

void check_values(EngineHarness &harness, const FormCase &form,
	std::uint64_t left, std::uint64_t right, std::uint64_t addend,
	unsigned value_index)
{
	constexpr unsigned left_register = 13;
	constexpr unsigned right_register = 27;
	constexpr unsigned addend_register = 19;
	constexpr unsigned destination = 7;
	const auto instruction = encode(form, left_register, right_register,
		is_high(form) ? 31U : addend_register, destination);
	auto initial = seeded_state(
		0x2000'0000'0000'0000ULL ^
		(static_cast<std::uint64_t>(value_index) << 44) ^ instruction);
	initial.registers[left_register] = left;
	initial.registers[right_register] = right;
	initial.registers[addend_register] = addend;
	initial.nzcv = 0xf000'0000U;
	char name[144];
	std::snprintf(name, sizeof(name),
		"%.*s arithmetic boundary case=%u",
		static_cast<int>(form.name.size()), form.name.data(), value_index);
	check(harness, name, instruction, initial);
}

void regular_boundaries(EngineHarness &harness)
{
	constexpr std::array values{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{2},
		std::uint64_t{3},
		std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0xffff'ffff'0000'0000ULL},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
	};
	constexpr std::array addends{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL},
	};
	for (unsigned form_index = 0; form_index < 4; ++form_index) {
		const auto &form = legal_forms[form_index];
		unsigned value_index = 0;
		for (const auto left : values)
			for (const auto right : values)
				for (const auto addend : addends)
					check_values(harness, form, left, right,
						addend, value_index++);
	}
}

void long_boundaries(EngineHarness &harness)
{
	constexpr std::array values{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{2},
		std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0xffff'ffff'8000'0000ULL},
		std::uint64_t{0x0000'0001'0000'0001ULL},
		std::uint64_t{0x89ab'cdef'0123'4567ULL},
	};
	constexpr std::array addends{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL},
		std::uint64_t{0xffff'ffff'0000'0000ULL},
		std::uint64_t{0x0000'0000'ffff'ffffULL},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
	};
	for (unsigned form_index = 4; form_index < 8; ++form_index) {
		const auto &form = legal_forms[form_index];
		unsigned value_index = 0;
		for (const auto left : values)
			for (const auto right : values)
				for (const auto addend : addends)
					check_values(harness, form, left, right,
						addend, value_index++);
	}
}

void high_half_boundaries(EngineHarness &harness)
{
	constexpr std::array values{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{2},
		std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0xffff'ffff'0000'0000ULL},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
	};
	for (unsigned form_index = 8; form_index < legal_forms.size();
	     ++form_index) {
		const auto &form = legal_forms[form_index];
		unsigned value_index = 0;
		for (const auto left : values)
			for (const auto right : values)
				check_values(harness, form, left, right, 0,
					value_index++);
	}
}

[[nodiscard]] constexpr std::string_view alias_name(
	const FormCase &form) noexcept
{
	if (form.operation == 0U)
		return form.subtract ? "MNEG" : "MUL";
	if (form.operation == 1U)
		return form.subtract ? "SMNEGL" : "SMULL";
	return form.subtract ? "UMNEGL" : "UMULL";
}

void named_product_aliases(EngineHarness &harness)
{
	for (unsigned form_index = 0; form_index < 8; ++form_index) {
		const auto &form = legal_forms[form_index];
		const auto alias = alias_name(form);
		for (unsigned reg = 0; reg < 32; ++reg) {
			const std::array patterns{
				std::array{reg, 11U, 23U},
				std::array{11U, reg, 23U},
				std::array{11U, 13U, reg},
				std::array{reg, reg, reg},
			};
			for (unsigned pattern = 0; pattern < patterns.size();
			     ++pattern) {
				const auto &registers = patterns[pattern];
				const auto instruction = encode(
					form, registers[0], registers[1], 31,
					registers[2]);
				auto initial = seeded_state(
					0x2100'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(
						form_index) << 48) ^
					(static_cast<std::uint64_t>(reg) << 40) ^
					pattern);
				char name[128];
				std::snprintf(name, sizeof(name),
					"%.*s alias rn=%u rm=%u rd=%u pattern=%u",
					static_cast<int>(alias.size()), alias.data(),
					registers[0], registers[1], registers[2],
					pattern);
				check(harness, name, instruction, initial);
			}
		}
	}
}

void zero_and_discard_semantics(EngineHarness &harness)
{
	for (unsigned form_index = 0; form_index < legal_forms.size();
	     ++form_index) {
		const auto &form = legal_forms[form_index];
		for (const bool left_zero : {false, true})
			for (const bool right_zero : {false, true})
				for (const bool addend_zero : {false, true})
					for (const bool discard : {false, true}) {
						if (is_high(form) && !addend_zero)
							continue;
						const unsigned left =
							left_zero ? 31U : 3U;
						const unsigned right =
							right_zero ? 31U : 5U;
						const unsigned addend =
							addend_zero ? 31U : 11U;
						const unsigned destination =
							discard ? 31U : 7U;
						const auto instruction = encode(
							form, left, right, addend,
							destination);
						auto initial = seeded_state(
							0x2200'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(
								form_index) << 48) ^
							(static_cast<std::uint64_t>(
								instruction) << 13));
						char name[160];
						std::snprintf(name, sizeof(name),
							"%.*s zero/discard rn=%u rm=%u "
							"ra=%u rd=%u",
							static_cast<int>(
								form.name.size()),
							form.name.data(), left, right,
							addend, destination);
						check(harness, name, instruction, initial);
					}
	}
}

void destructive_register_aliases(EngineHarness &harness)
{
	constexpr std::array nonhigh_patterns{
		std::array{7U, 2U, 3U, 7U},
		std::array{1U, 7U, 3U, 7U},
		std::array{1U, 2U, 7U, 7U},
		std::array{7U, 7U, 3U, 2U},
		std::array{7U, 2U, 7U, 3U},
		std::array{1U, 7U, 7U, 2U},
		std::array{7U, 7U, 7U, 7U},
	};
	constexpr std::array high_patterns{
		std::array{7U, 2U, 31U, 7U},
		std::array{1U, 7U, 31U, 7U},
		std::array{7U, 7U, 31U, 2U},
		std::array{7U, 7U, 31U, 7U},
	};
	for (unsigned form_index = 0; form_index < legal_forms.size();
	     ++form_index) {
		const auto &form = legal_forms[form_index];
		const auto run = [&](const auto &patterns) {
			for (unsigned pattern = 0; pattern < patterns.size();
			     ++pattern) {
				const auto &registers = patterns[pattern];
				const auto instruction = encode(
					form, registers[0], registers[1],
					registers[2], registers[3]);
				auto initial = seeded_state(
					0x2300'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(
						form_index) << 48) ^
					pattern);
				char name[128];
				std::snprintf(name, sizeof(name),
					"%.*s destructive register pattern=%u",
					static_cast<int>(form.name.size()),
					form.name.data(), pattern);
				check(harness, name, instruction, initial);
			}
		};
		if (is_high(form))
			run(high_patterns);
		else
			run(nonhigh_patterns);
	}
}

} // namespace

void run_behavior_tests(EngineHarness &harness)
{
	regular_boundaries(harness);
	long_boundaries(harness);
	high_half_boundaries(harness);
	named_product_aliases(harness);
	zero_and_discard_semantics(harness);
	destructive_register_aliases(harness);
}

} // namespace uml::backend::tests::base_multiply
