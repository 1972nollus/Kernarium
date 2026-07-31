// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_multiply_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <set>

namespace uml::backend::tests::base_multiply {
namespace {

namespace gadgets = arm64::gadgets;

struct Registers final {
	unsigned left;
	unsigned right;
	unsigned addend;
	unsigned destination;
	std::string_view name;
};

[[noreturn]] void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

void run_case(EngineHarness &harness, const FormCase &form,
	const Registers &registers, std::uint64_t seed)
{
	const auto instruction = encode(form, registers.left, registers.right,
		registers.addend, registers.destination);
	auto initial = seeded_state(seed ^ instruction);
	char name[144];
	std::snprintf(name, sizeof(name),
		"%.*s register allocation rn=%u rm=%u ra=%u rd=%u %.*s",
		static_cast<int>(form.name.size()), form.name.data(),
		registers.left, registers.right, registers.addend,
		registers.destination,
		static_cast<int>(registers.name.size()), registers.name.data());
	check(harness, name, instruction, initial);
}

void every_register_field(EngineHarness &harness)
{
	for (unsigned form_index = 0; form_index < legal_forms.size();
	     ++form_index) {
		const auto &form = legal_forms[form_index];
		for (unsigned reg = 0; reg < 32; ++reg) {
			const std::array common{
				Registers{reg, 11, is_high(form) ? 31U : 17U, 23,
					"rn"},
				Registers{11, reg, is_high(form) ? 31U : 17U, 23,
					"rm"},
				Registers{11, 13, is_high(form) ? 31U : 17U, reg,
					"rd"},
				Registers{reg, reg, is_high(form) ? 31U : reg, reg,
					"all-legal-alias"},
			};
			for (const auto &registers : common)
				run_case(harness, form, registers,
					0x1000'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(
						form_index) << 48) ^
					(static_cast<std::uint64_t>(reg) << 40));
			if (!is_high(form))
				run_case(harness, form,
					Registers{11, 13, reg, 23, "ra"},
					0x1100'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(
						form_index) << 48) ^
					(static_cast<std::uint64_t>(reg) << 40));
		}
	}
}

void every_legal_decode_partition(EngineHarness &harness)
{
	std::size_t valid_partitions = 0;
	for (unsigned sf = 0; sf < 2; ++sf)
		for (unsigned op54 = 0; op54 < 4; ++op54)
			for (unsigned operation = 0; operation < 8; ++operation)
				for (unsigned subtract = 0; subtract < 2; ++subtract)
					for (unsigned addend = 0; addend < 32; ++addend) {
						const unsigned left =
							(addend * 3U + operation) & 31U;
						const unsigned right =
							(addend * 7U + subtract) & 31U;
						const unsigned destination =
							(addend * 13U + sf) & 31U;
						const auto instruction = encode(
							sf != 0U, op54, operation,
							subtract != 0U, right, addend,
							left, destination);
						if (!is_valid(instruction))
							continue;
						++valid_partitions;
						auto initial = seeded_state(
							0x1200'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(
								instruction) << 17));
						char name[152];
						std::snprintf(name, sizeof(name),
							"multiply valid partition sf=%u "
							"op54=%u op31=%u o0=%u ra=%u",
							sf, op54, operation, subtract,
							addend);
						check(harness, name, instruction, initial);
					}
	require(valid_partitions == 258U,
		"multiply valid partition count does not match specification");
}

void production_specialization()
{
	constexpr auto base = engine_test_code_base;
	constexpr std::uint32_t nop = 0xd503'201fU;
	const auto folded_nop = gadgets::Builder{}.build(base, std::array{nop});
	require(folded_nop.complete, "NOP program did not build");

	const auto entry_for = [](const FormCase &form, unsigned left,
				       unsigned right, unsigned addend) {
		const auto instruction = encode(form, left, right, addend, 7U);
		const auto program =
			gadgets::Builder{}.build(base, std::array{instruction});
		require(program.complete && program.program.words().size() == 4U,
			"multiply selector probe has unexpected stream shape");
		return program.program.words().front();
	};

	std::set<std::uintptr_t> entries;
	for (const auto &form : legal_forms)
		for (const bool left_zero : {false, true})
			for (const bool right_zero : {false, true})
				for (const bool addend_zero : {false, true}) {
					if (is_high(form) && !addend_zero)
						continue;
					const auto instruction = encode(
						form, left_zero ? 31U : 3U,
						right_zero ? 31U : 5U,
						addend_zero ? 31U : 11U, 7U);
					const auto program = gadgets::Builder{}.build(
						base, std::array{instruction});
					require(program.complete &&
							program.program.words().size() == 4U,
						"multiply form has unexpected specialized "
						"stream shape");
					const auto operand = program.program.words()[1];
					require((operand & 0xffffU) ==
							7U * sizeof(std::uint64_t),
						"multiply destination was not resolved at "
						"translation time");
					entries.insert(program.program.words().front());

					const auto discard_instruction =
						(instruction & ~31U) | 31U;
					const auto discarded = gadgets::Builder{}.build(
						base, std::array{discard_instruction});
					require(discarded.complete &&
							discarded.program.words().size() == 3U,
						"discarded multiply retained runtime operands");
					require(discarded.program.words().front() ==
							folded_nop.program.words().front(),
						"discarded multiply was not folded to NOP");
				}
	require(entries.size() == 36U,
		"multiply source/addend forms did not select exactly 36 live gadgets");

	for (const auto &form : legal_forms)
		for (const bool addend_zero : {false, true}) {
			if (is_high(form) && !addend_zero)
				continue;
			const unsigned addend = addend_zero ? 31U : 11U;
			const auto ordinary = entry_for(form, 3U, 5U, addend);
			const auto left_zero = entry_for(form, 31U, 5U, addend);
			const auto right_zero = entry_for(form, 3U, 31U, addend);
			const auto both_zero = entry_for(form, 31U, 31U, addend);
			require(ordinary != left_zero,
				"ordinary multiply selected the zero-product gadget");
			require(left_zero == right_zero && left_zero == both_zero,
				"zero-product multiply did not share one gadget");
		}
}

} // namespace

void run_allocation_tests(EngineHarness &harness)
{
	every_register_field(harness);
	every_legal_decode_partition(harness);
	production_specialization();
}

} // namespace uml::backend::tests::base_multiply
