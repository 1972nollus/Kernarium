// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

#include <set>

namespace pauth = arm64_pauth_test;
namespace test = uml::emulator::tests;

namespace {

constexpr std::size_t target_offset = 16;
constexpr std::uint64_t target_address =
	test::engine_test_code_base + target_offset;

void validate_guarded_return_fault(
	const pauth::arm64::CpuState &initial,
	const pauth::arm64::CpuState &expected_template,
	const pauth::arm64::CpuState &actual,
	std::string_view case_name, std::string_view engine,
	std::uint32_t instruction)
{
	auto expected = initial;
	expected.pc = expected_template.pc;
	pauth::require(
		pauth::strip(actual.registers[30], false) ==
		initial.registers[30]);
	expected.registers[30] = actual.registers[30];
	test::require_architecture_equal(
		expected, actual, case_name, engine, instruction);
}

void return_forms(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (unsigned salt = 0; salt < 16; ++salt) {
			auto initial = pauth::initial_state(
				static_cast<unsigned>(key_b) * 32 + salt);
			initial.registers[30] = target_address;
			const std::array code{
				test::ProgramWord{
					.offset = 0,
					.instruction = pauth::data_processing(
						key_b ? 1U : 0U, 30, 31)},
				test::ProgramWord{
					.offset = 4,
					.instruction =
						pauth::authenticated_return(key_b)},
				test::ProgramWord{
					.offset = target_offset,
					.instruction = pauth::xpac(false, 30)},
				test::ProgramWord{
					.offset = target_offset + 4,
					.instruction = 0xd400'0001U},
			};
			auto expected = initial;
			expected.registers[30] = target_address;
			expected.pc = target_address + 8;
			const pauth::CaseName name{
				"ret%s-sp-s%u", key_b ? "ab" : "aa", salt};
			harness.check_program({
				.name = name,
				.code = code,
				.initial = initial,
				.expected = expected,
				.native = pauth::native_pauth,
			});
			++cases;
		}
	}
}

void exception_return_is_illegal_at_el0(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (unsigned salt = 0; salt < 16; ++salt) {
			const auto initial = pauth::initial_state(
				0x100 + static_cast<unsigned>(key_b) * 32 + salt);
			const auto instruction =
				key_b ? 0xd69f'0fffU : 0xd69f'0bffU;
			const pauth::CaseName name{
				"eret%s-el0-s%u", key_b ? "ab" : "aa", salt};
			harness.check({
				.name = name,
				.instruction = instruction,
				.initial = initial,
				.expected = initial,
				.event =
					test::ExpectedEvent::illegal_instruction,
				.native = pauth::native_pauth,
			});
			++cases;
		}
	}
}

void zero_target_return(test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		auto initial = pauth::initial_state(
			0x180 + static_cast<unsigned>(key_b));
		initial.registers[30] = 0;
		const pauth::CaseName name{
			"ret%s-zero-target", key_b ? "ab" : "aa"};
		harness.check({
			.name = name,
			.instruction = pauth::authenticated_return(key_b),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::memory_fault,
			.native = pauth::native_pauth,
			.validator = &pauth::validate_branch_failure,
			.validator_relation_id =
				"permitted-transformed-branch-failure/v1",
			.expected_instructions = 1,
		});
		++cases;
	}
}

void guarded_return_success(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		pauth::Page initial_memory;
		pauth::fill_page(
			initial_memory, 81U + static_cast<unsigned>(key_b));
		auto initial = pauth::initial_state(
			0xc00U + static_cast<unsigned>(key_b));
		initial.gcs_implemented = true;
		initial.gcs_push_enabled = true;
		initial.gcs_pointer =
			(pauth::data_base + pauth::page_size / 2) &
			~std::uint64_t{7};
		initial.registers[30] = target_address;
		pauth::store_u64(
			initial_memory,
			static_cast<std::size_t>(
				initial.gcs_pointer - pauth::data_base),
			target_address);
		const auto expected_memory = initial_memory;
		const std::array regions{
			pauth::memory_region(initial_memory, expected_memory),
		};
		const std::array code{
			test::ProgramWord{
				.offset = 0,
				.instruction = pauth::data_processing(
					key_b ? 1U : 0U, 30, 31)},
			test::ProgramWord{
				.offset = 4,
				.instruction =
					pauth::authenticated_return(key_b)},
			test::ProgramWord{
				.offset = target_offset,
				.instruction = pauth::xpac(false, 30)},
			test::ProgramWord{
				.offset = target_offset + 4,
				.instruction = 0xd400'0001U},
		};
		auto expected = initial;
		expected.registers[30] = target_address;
		expected.gcs_pointer = initial.gcs_pointer + 8;
		expected.pc = target_address + 8;
		const pauth::CaseName name{
			"ret-guarded-success-%c", key_b ? 'b' : 'a'};
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

void guarded_return_disablement(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool feature : {false, true}) {
		const bool key_b = feature;
		auto initial = pauth::initial_state(
			0xd00U + static_cast<unsigned>(feature));
		initial.gcs_implemented = feature;
		initial.gcs_push_enabled = !feature;
		initial.gcs_pointer =
			pauth::data_base + 3 * pauth::page_size;
		initial.registers[30] = target_address;
		const std::array code{
			test::ProgramWord{
				.offset = 0,
				.instruction = pauth::data_processing(
					key_b ? 1U : 0U, 30, 31)},
			test::ProgramWord{
				.offset = 4,
				.instruction =
					pauth::authenticated_return(key_b)},
			test::ProgramWord{
				.offset = target_offset,
				.instruction = pauth::xpac(false, 30)},
			test::ProgramWord{
				.offset = target_offset + 4,
				.instruction = 0xd400'0001U},
		};
		auto expected = initial;
		expected.registers[30] = target_address;
		expected.pc = target_address + 8;
		const pauth::CaseName name{
			"ret-guarded-disabled-f%u",
			static_cast<unsigned>(feature)};
		harness.check_program({
			.name = name,
			.code = code,
			.initial = initial,
			.expected = expected,
			.native = pauth::native_pauth,
		});
		++cases;
	}
}

void guarded_return_mismatch(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		pauth::Page initial_memory;
		pauth::fill_page(
			initial_memory, 101U + static_cast<unsigned>(key_b));
		auto initial = pauth::initial_state(
			0xe00U + static_cast<unsigned>(key_b));
		initial.gcs_implemented = true;
		initial.gcs_push_enabled = true;
		initial.gcs_pointer =
			(pauth::data_base + pauth::page_size / 2) &
			~std::uint64_t{7};
		initial.registers[30] = target_address;
		pauth::store_u64(
			initial_memory,
			static_cast<std::size_t>(
				initial.gcs_pointer - pauth::data_base),
			target_address + 8);
		const auto expected_memory = initial_memory;
		const std::array regions{
			pauth::memory_region(initial_memory, expected_memory),
		};
		const std::array code{
			test::ProgramWord{
				.offset = 0,
				.instruction = pauth::data_processing(
					key_b ? 1U : 0U, 30, 31)},
			test::ProgramWord{
				.offset = 4,
				.instruction =
					pauth::authenticated_return(key_b)},
		};
		auto expected = initial;
		expected.pc = test::engine_test_code_base + 4;
		const pauth::CaseName name{
			"ret-guarded-mismatch-%c", key_b ? 'b' : 'a'};
		harness.check_program({
			.name = name,
			.code = code,
			.initial = initial,
			.expected = expected,
			.event = test::ExpectedEvent::gcs_fault,
			.native = pauth::native_pauth,
			.memory = regions,
			.validator = &validate_guarded_return_fault,
			.validator_relation_id =
				"guarded-control-return-fault/v1",
		});
		++cases;
	}
}

void guarded_return_fault_atomicity(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		pauth::Page initial_memory;
		pauth::fill_page(
			initial_memory, 121U + static_cast<unsigned>(key_b));
		const auto expected_memory = initial_memory;
		auto initial = pauth::initial_state(
			0xf00U + static_cast<unsigned>(key_b));
		initial.gcs_implemented = true;
		initial.gcs_push_enabled = true;
		initial.gcs_pointer =
			(pauth::data_base + pauth::page_size / 2) &
			~std::uint64_t{7};
		initial.registers[30] = target_address;
		const std::array regions{
			pauth::memory_region(
				initial_memory, expected_memory, false, false),
		};
		const std::array code{
			test::ProgramWord{
				.offset = 0,
				.instruction = pauth::data_processing(
					key_b ? 1U : 0U, 30, 31)},
			test::ProgramWord{
				.offset = 4,
				.instruction =
					pauth::authenticated_return(key_b)},
		};
		auto expected = initial;
		expected.pc = test::engine_test_code_base + 4;
		const pauth::CaseName name{
			"ret-guarded-read-fault-%c", key_b ? 'b' : 'a'};
		harness.check_program({
			.name = name,
			.code = code,
			.initial = initial,
			.expected = expected,
			.event = test::ExpectedEvent::memory_fault,
			.native = pauth::native_pauth,
			.fault = {
				.address = initial.gcs_pointer,
				.access = pauth::arm64::MemoryAccess::read,
			},
			.compare_fault = true,
			.memory = regions,
			.validator = &validate_guarded_return_fault,
			.validator_relation_id =
				"guarded-control-return-fault/v1",
		});
		++cases;
	}
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool key_b : {false, true}) {
		const auto built = pauth::gadgets::Builder{}.build(
			pauth::pc,
			std::array{
				pauth::authenticated_return(key_b)});
		pauth::require(
			built.complete &&
			built.program.instruction_count() == 1);
		entries.insert(built.program.words().front());
	}
	pauth::require(entries.size() == 2);
	std::set<std::uintptr_t> guarded_entries;
	for (const bool key_b : {false, true}) {
		const auto built = pauth::gadgets::Builder{}.build(
			pauth::pc,
			std::array{pauth::authenticated_return(key_b)},
			{}, std::pmr::get_default_resource(), {},
			pauth::gadgets::BuildOptions{
				.enable_guarded_control_flow = true});
		pauth::require(
			built.complete &&
			built.program.instruction_count() == 1);
		const auto entry = built.program.words().front();
		pauth::require(entries.count(entry) == 0);
		guarded_entries.insert(entry);
	}
	pauth::require(guarded_entries.size() == 2);

	const auto eretaa = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{0xd69f'0bffU});
	const auto eretab = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{0xd69f'0fffU});
	const auto illegal = pauth::gadgets::Builder{}.build(
		pauth::pc, std::array{std::uint32_t{0}});
	pauth::require(
		eretaa.program.words().front() ==
			illegal.program.words().front() &&
		eretab.program.words().front() ==
			illegal.program.words().front());
}

} // namespace

int main()
{
	test::EngineHarness harness;
	std::size_t cases = 0;
	return_forms(harness, cases);
	exception_return_is_illegal_at_el0(harness, cases);
	zero_target_return(harness, cases);
	production_specialization();
	pauth::require_counts(harness, cases);
	harness.print_summary("arm64-base-pauth-return");

	test::EngineHarness guarded_harness{
		test::EngineHarnessOptions{
			.enable_guarded_control_flow = true,
		}};
	std::size_t guarded_cases = 0;
	guarded_return_success(guarded_harness, guarded_cases);
	guarded_return_disablement(guarded_harness, guarded_cases);
	guarded_return_mismatch(guarded_harness, guarded_cases);
	guarded_return_fault_atomicity(
		guarded_harness, guarded_cases);
	pauth::require_counts(guarded_harness, guarded_cases);
	guarded_harness.print_summary(
		"arm64-base-pauth-return-guarded");
	return 0;
}
