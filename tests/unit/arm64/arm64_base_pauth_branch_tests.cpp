// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

#include <set>

namespace pauth = arm64_pauth_test;
namespace test = uml::emulator::tests;

namespace {

constexpr std::size_t target_offset = 16;
constexpr std::uint64_t target_address =
	test::engine_test_code_base + target_offset;
constexpr unsigned guarded_target = 3;
constexpr unsigned guarded_modifier = 7;

void validate_overlapping_branch_failure(
	const pauth::arm64::CpuState &initial,
	const pauth::arm64::CpuState &,
	const pauth::arm64::CpuState &actual,
	std::string_view case_name, std::string_view engine,
	std::uint32_t instruction, bool link)
{
	const unsigned target = instruction & 31U;
	pauth::require(pauth::strip(actual.pc, false) == target_address);
	auto expected = initial;
	expected.pc = actual.pc;
	if (!link || target != 30U) {
		pauth::require(
			pauth::strip(actual.registers[target], false) ==
			initial.registers[target]);
		expected.registers[target] = actual.registers[target];
	}
	if (link)
		expected.registers[30] = test::engine_test_code_base + 8;
	test::require_architecture_equal(
		expected, actual, case_name, engine, instruction);
}

void validate_overlapping_branch_failure_no_link(
	const pauth::arm64::CpuState &initial,
	const pauth::arm64::CpuState &expected,
	const pauth::arm64::CpuState &actual,
	std::string_view case_name, std::string_view engine,
	std::uint32_t instruction)
{
	validate_overlapping_branch_failure(
		initial, expected, actual, case_name, engine, instruction, false);
}

void validate_overlapping_branch_failure_link(
	const pauth::arm64::CpuState &initial,
	const pauth::arm64::CpuState &expected,
	const pauth::arm64::CpuState &actual,
	std::string_view case_name, std::string_view engine,
	std::uint32_t instruction)
{
	validate_overlapping_branch_failure(
		initial, expected, actual, case_name, engine, instruction, true);
}

void validate_guarded_call_fault(
	const pauth::arm64::CpuState &initial,
	const pauth::arm64::CpuState &expected_template,
	const pauth::arm64::CpuState &actual,
	std::string_view case_name, std::string_view engine,
	std::uint32_t instruction)
{
	auto expected = initial;
	expected.pc = expected_template.pc;
	pauth::require(
		pauth::strip(actual.registers[guarded_target], false) ==
		initial.registers[guarded_target]);
	expected.registers[guarded_target] =
		actual.registers[guarded_target];
	test::require_architecture_equal(
		expected, actual, case_name, engine, instruction);
}

void validate_guarded_zero_target(
	const pauth::arm64::CpuState &,
	const pauth::arm64::CpuState &expected_template,
	const pauth::arm64::CpuState &actual,
	std::string_view case_name, std::string_view engine,
	std::uint32_t instruction)
{
	pauth::require(pauth::strip(actual.pc, false) == 0);
	auto expected = expected_template;
	expected.pc = actual.pc;
	test::require_architecture_equal(
		expected, actual, case_name, engine, instruction);
}

void check_success(
	test::EngineHarness &harness, std::string_view name,
	bool link, bool key_b, bool zero_modifier,
	unsigned target, unsigned modifier,
	const pauth::arm64::CpuState &initial)
{
	const auto sign = pauth::data_processing(
		(key_b ? 1U : 0U) | (zero_modifier ? 8U : 0U),
		target, zero_modifier ? 31U : modifier);
	const auto branch = pauth::authenticated_branch(
		link, key_b, zero_modifier, target,
		zero_modifier ? 31U : modifier);
	const std::array code{
		test::ProgramWord{.offset = 0, .instruction = sign},
		test::ProgramWord{.offset = 4, .instruction = branch},
		test::ProgramWord{
			.offset = target_offset,
			.instruction = pauth::xpac(false, target)},
		test::ProgramWord{
			.offset = target_offset + 4,
			.instruction = 0xd400'0001U},
	};
	auto expected = initial;
	expected.pc = target_address + 8;
	expected.registers[target] =
		pauth::strip(initial.registers[target], false);
	if (link)
		expected.registers[30] =
			test::engine_test_code_base + 8;
	harness.check_program({
		.name = name,
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = pauth::native_pauth,
	});
}

void check_overlapping_failure(
	test::EngineHarness &harness, std::string_view name,
	bool link, bool key_b, unsigned target,
	const pauth::arm64::CpuState &initial)
{
	const std::array code{
		test::ProgramWord{
			.offset = 0,
			.instruction = pauth::data_processing(
				key_b ? 1U : 0U, target, target)},
		test::ProgramWord{
			.offset = 4,
			.instruction = pauth::authenticated_branch(
				link, key_b, false, target, target)},
	};
	harness.check_program({
		.name = name,
		.code = code,
		.initial = initial,
		.expected = initial,
		.event = test::ExpectedEvent::memory_fault,
		.native = pauth::native_pauth,
		.validator = link ?
			&validate_overlapping_branch_failure_link :
			&validate_overlapping_branch_failure_no_link,
		.validator_relation_id =
			"permitted-transformed-branch-failure/v1",
		.expected_instructions = 2,
	});
}

void register_modifier_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool link : {false, true}) {
		for (const bool key_b : {false, true}) {
			for (unsigned target = 0; target < 31; ++target) {
				for (unsigned modifier = 0;
				     modifier < 32; ++modifier) {
					auto initial = pauth::initial_state(
						static_cast<unsigned>(link) * 8192 +
						static_cast<unsigned>(key_b) * 4096 +
						target * 128 + modifier);
					initial.registers[target] = target_address;
					const pauth::CaseName name{
						"%s-%s-%c-t%u-m%u",
						link ? "blra" : "bra",
						target == modifier ?
							"overlap" : "register",
						key_b ? 'b' : 'a',
						target, modifier};
					if (target == modifier)
						check_overlapping_failure(
							harness, name, link, key_b,
							target, initial);
					else
						check_success(
							harness, name, link, key_b,
							false, target, modifier,
							initial);
					++cases;
				}
			}
		}
	}
}

void zero_modifier_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool link : {false, true}) {
		for (const bool key_b : {false, true}) {
			for (unsigned target = 0; target < 31; ++target) {
				auto initial = pauth::initial_state(
					static_cast<unsigned>(link) * 256 +
					static_cast<unsigned>(key_b) * 128 + target);
				initial.registers[target] = target_address;
				const pauth::CaseName name{
					"%s-zero-%c-t%u",
					link ? "blra" : "bra",
					key_b ? 'b' : 'a', target};
				check_success(
					harness, name, link, key_b, true,
					target, 31, initial);
				++cases;
			}
		}
	}
}

void zero_target_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool link : {false, true}) {
		for (const bool key_b : {false, true}) {
			for (const bool zero_modifier : {false, true}) {
				const unsigned modifier =
					zero_modifier ? 31U : 7U;
				const auto instruction =
					pauth::authenticated_branch(
						link, key_b, zero_modifier,
						31, modifier);
				const auto initial = pauth::initial_state(
					static_cast<unsigned>(link) * 8 +
					static_cast<unsigned>(key_b) * 4 +
					static_cast<unsigned>(zero_modifier));
				const pauth::CaseName name{
					"%s-xzr-%c-z%u",
					link ? "blra" : "bra",
					key_b ? 'b' : 'a',
					static_cast<unsigned>(zero_modifier)};
				harness.check({
					.name = name,
					.instruction = instruction,
					.initial = initial,
					.expected = initial,
					.event = test::ExpectedEvent::memory_fault,
					.native = pauth::native_pauth,
					.validator =
						&pauth::validate_branch_failure,
					.validator_relation_id =
						"permitted-transformed-branch-failure/v1",
					.expected_instructions = 1,
				});
				++cases;
			}
		}
	}
}

void reserved_zero_modifier_encodings(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool link : {false, true}) {
		for (const bool key_b : {false, true}) {
			for (unsigned modifier = 0;
			     modifier < 31; ++modifier) {
				for (unsigned target = 0;
				     target < 32; ++target) {
					const pauth::CaseName name{
						"%s-reserved-%c-m%u-t%u",
						link ? "blra" : "bra",
						key_b ? 'b' : 'a',
						modifier, target};
					harness.check_illegal(
						name,
						pauth::authenticated_branch(
							link, key_b, true,
							target, modifier),
						pauth::initial_state(
							static_cast<unsigned>(link) *
								4096 +
							static_cast<unsigned>(key_b) *
								2048 +
							modifier * 32 + target),
						pauth::native_pauth);
					++cases;
				}
			}
		}
	}
}

void guarded_call_success(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (const bool zero_modifier : {false, true}) {
			pauth::Page initial_memory;
			pauth::fill_page(initial_memory,
				41U + static_cast<unsigned>(key_b) * 2U +
				static_cast<unsigned>(zero_modifier));
			auto expected_memory = initial_memory;
			auto initial = pauth::initial_state(
				0x900U + static_cast<unsigned>(key_b) * 2U +
				static_cast<unsigned>(zero_modifier));
			initial.gcs_implemented = true;
			initial.gcs_push_enabled = true;
			initial.gcs_pointer =
				(pauth::data_base + pauth::page_size / 2) &
				~std::uint64_t{7};
			initial.registers[guarded_target] = target_address;
			const auto record_address = initial.gcs_pointer - 8;
			pauth::store_u64(
				expected_memory,
				static_cast<std::size_t>(
					record_address - pauth::data_base),
				test::engine_test_code_base + 8);
			const std::array regions{
				pauth::memory_region(
					initial_memory, expected_memory),
			};
			const std::array code{
				test::ProgramWord{
					.offset = 0,
					.instruction = pauth::data_processing(
						(key_b ? 1U : 0U) |
							(zero_modifier ? 8U : 0U),
						guarded_target,
						zero_modifier ? 31U :
							guarded_modifier)},
				test::ProgramWord{
					.offset = 4,
					.instruction =
						pauth::authenticated_branch(
							true, key_b,
							zero_modifier,
							guarded_target,
							zero_modifier ? 31U :
								guarded_modifier)},
				test::ProgramWord{
					.offset = target_offset,
					.instruction =
						pauth::xpac(
							false,
							guarded_target)},
				test::ProgramWord{
					.offset = target_offset + 4,
					.instruction = 0xd400'0001U},
			};
			auto expected = initial;
			expected.registers[guarded_target] = target_address;
			expected.registers[30] =
				test::engine_test_code_base + 8;
			expected.gcs_pointer = record_address;
			expected.pc = target_address + 8;
			const pauth::CaseName name{
				"blra-guarded-success-%c-z%u",
				key_b ? 'b' : 'a',
				static_cast<unsigned>(zero_modifier)};
			harness.check_program({
				.name = name,
				.code = code,
				.initial = initial,
				.expected = expected,
				.native = pauth::native_pauth,
				.memory = regions,
			});
			++cases;
		}
	}
}

void guarded_call_disablement(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool feature : {false, true}) {
		auto initial = pauth::initial_state(
			0xa00U + static_cast<unsigned>(feature));
		initial.gcs_implemented = feature;
		initial.gcs_push_enabled = !feature;
		initial.gcs_pointer =
			pauth::data_base + 3 * pauth::page_size;
		initial.registers[guarded_target] = target_address;
		const pauth::CaseName name{
			"blra-guarded-disabled-f%u",
			static_cast<unsigned>(feature)};
		check_success(
			harness, name, true, feature, false,
			guarded_target, guarded_modifier, initial);
		++cases;
	}
	for (const bool key_b : {false, true}) {
		auto initial = pauth::initial_state(
			0xa10U + static_cast<unsigned>(key_b));
		initial.gcs_implemented = true;
		initial.gcs_push_enabled = true;
		initial.gcs_pointer =
			pauth::data_base + 3 * pauth::page_size;
		initial.registers[guarded_target] = target_address;
		const pauth::CaseName name{
			"bra-guarded-never-push-%c",
			key_b ? 'b' : 'a'};
		check_success(
			harness, name, false, key_b, false,
			guarded_target, guarded_modifier, initial);
		++cases;
	}
}

void guarded_call_zero_target(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (const bool zero_modifier : {false, true}) {
			pauth::Page initial_memory;
			pauth::fill_page(initial_memory,
				51U + static_cast<unsigned>(key_b) * 2U +
				static_cast<unsigned>(zero_modifier));
			auto expected_memory = initial_memory;
			auto initial = pauth::initial_state(
				0xa20U + static_cast<unsigned>(key_b) * 2U +
				static_cast<unsigned>(zero_modifier));
			initial.gcs_implemented = true;
			initial.gcs_push_enabled = true;
			initial.gcs_pointer =
				(pauth::data_base + pauth::page_size / 2) &
				~std::uint64_t{7};
			const auto record_address = initial.gcs_pointer - 8;
			pauth::store_u64(
				expected_memory,
				static_cast<std::size_t>(
					record_address - pauth::data_base),
				test::engine_test_code_base + 4);
			const std::array regions{
				pauth::memory_region(
					initial_memory, expected_memory),
			};
			const std::array code{
				test::ProgramWord{
					.offset = 0,
					.instruction =
						pauth::authenticated_branch(
							true, key_b,
							zero_modifier, 31,
							zero_modifier ? 31U :
								guarded_modifier)},
			};
			auto expected = initial;
			expected.registers[30] =
				test::engine_test_code_base + 4;
			expected.gcs_pointer = record_address;
			const pauth::CaseName name{
				"blra-guarded-xzr-%c-z%u",
				key_b ? 'b' : 'a',
				static_cast<unsigned>(zero_modifier)};
			harness.check_program({
				.name = name,
				.code = code,
				.initial = initial,
				.expected = expected,
				.event = test::ExpectedEvent::memory_fault,
				.native = pauth::native_pauth,
				.memory = regions,
				.validator = &validate_guarded_zero_target,
				.validator_relation_id =
					"guarded-control-zero-target-fault/v1",
			});
			++cases;
		}
	}
}

void guarded_call_fault_atomicity(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (const bool zero_modifier : {false, true}) {
			pauth::Page initial_memory;
			pauth::fill_page(initial_memory,
				61U + static_cast<unsigned>(key_b) * 2U +
				static_cast<unsigned>(zero_modifier));
			const auto expected_memory = initial_memory;
			auto initial = pauth::initial_state(
				0xb00U + static_cast<unsigned>(key_b) * 2U +
				static_cast<unsigned>(zero_modifier));
			initial.gcs_implemented = true;
			initial.gcs_push_enabled = true;
			initial.gcs_pointer =
				(pauth::data_base + pauth::page_size / 2) &
				~std::uint64_t{7};
			initial.registers[guarded_target] = target_address;
			const auto record_address = initial.gcs_pointer - 8;
			const std::array regions{
				pauth::memory_region(
					initial_memory, expected_memory, true, false),
			};
			const std::array code{
				test::ProgramWord{
					.offset = 0,
					.instruction = pauth::data_processing(
						(key_b ? 1U : 0U) |
							(zero_modifier ? 8U : 0U),
						guarded_target,
						zero_modifier ? 31U :
							guarded_modifier)},
				test::ProgramWord{
					.offset = 4,
					.instruction =
						pauth::authenticated_branch(
							true, key_b,
							zero_modifier,
							guarded_target,
							zero_modifier ? 31U :
								guarded_modifier)},
			};
			auto expected = initial;
			expected.pc = test::engine_test_code_base + 4;
			const pauth::CaseName name{
				"blra-guarded-write-fault-%c-z%u",
				key_b ? 'b' : 'a',
				static_cast<unsigned>(zero_modifier)};
			harness.check_program({
				.name = name,
				.code = code,
				.initial = initial,
				.expected = expected,
				.event = test::ExpectedEvent::memory_fault,
				.native = pauth::native_pauth,
				.fault = {
					.address = record_address,
					.access =
						pauth::arm64::MemoryAccess::write,
				},
				.compare_fault = true,
				.memory = regions,
				.validator = &validate_guarded_call_fault,
				.validator_relation_id =
					"guarded-control-call-fault-atomicity/v1",
			});
			++cases;
		}
	}
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool link : {false, true}) {
		for (const bool key_b : {false, true}) {
			for (const bool target_zero : {false, true}) {
				for (const bool modifier_zero : {false, true}) {
					const auto built =
						pauth::gadgets::Builder{}.build(
							pauth::pc,
							std::array{
								pauth::authenticated_branch(
									link, key_b,
									modifier_zero,
									target_zero ? 31U : 3U,
									modifier_zero ? 31U :
										7U)});
					pauth::require(
						built.complete &&
						built.program.instruction_count() ==
							1);
					entries.insert(
						built.program.words().front());
				}
			}
		}
	}
	pauth::require(entries.size() == 16);

	std::set<std::uintptr_t> guarded_entries;
	for (const bool key_b : {false, true}) {
		for (const bool target_zero : {false, true}) {
			for (const bool modifier_zero : {false, true}) {
				const auto built =
					pauth::gadgets::Builder{}.build(
						pauth::pc,
						std::array{
							pauth::authenticated_branch(
								true, key_b,
								modifier_zero,
								target_zero ? 31U :
									3U,
								modifier_zero ? 31U :
									7U)},
						{}, std::pmr::get_default_resource(),
						{},
						pauth::gadgets::BuildOptions{
							.enable_guarded_control_flow =
								true});
				pauth::require(
					built.complete &&
					built.program.instruction_count() == 1);
				const auto entry = built.program.words().front();
				pauth::require(entries.count(entry) == 0);
				guarded_entries.insert(entry);
			}
		}
	}
	pauth::require(guarded_entries.size() == 8);
}

} // namespace

int main()
{
	test::EngineHarness harness;
	std::size_t cases = 0;
	register_modifier_forms(harness, cases);
	zero_modifier_forms(harness, cases);
	zero_target_forms(harness, cases);
	reserved_zero_modifier_encodings(harness, cases);
	production_specialization();
	pauth::require_counts(harness, cases);
	harness.print_summary("arm64-base-pauth-branch");

	test::EngineHarness guarded_harness{
		test::EngineHarnessOptions{
			.enable_guarded_control_flow = true,
		}};
	std::size_t guarded_cases = 0;
	guarded_call_success(guarded_harness, guarded_cases);
	guarded_call_disablement(guarded_harness, guarded_cases);
	guarded_call_zero_target(guarded_harness, guarded_cases);
	guarded_call_fault_atomicity(guarded_harness, guarded_cases);
	pauth::require_counts(guarded_harness, guarded_cases);
	guarded_harness.print_summary(
		"arm64-base-pauth-branch-guarded");
	return 0;
}
