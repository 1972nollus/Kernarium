// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

#include "uml_darwin_guest.h"

#include <cstring>
#include <set>
#include <vector>

namespace pauth = arm64_pauth_test;
namespace test = uml::emulator::tests;

namespace {

constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::uint64_t payload = 0xe817'4675'a4d3'fe81ULL;
constexpr unsigned signed_fault_base = 3;
constexpr unsigned signed_fault_destination = 7;
constexpr unsigned signed_fault_encoded_offset = 1;
constexpr std::uint64_t signed_fault_plain_base = 0x7a00'1000ULL;

[[nodiscard]] constexpr std::uint32_t move_register(
	unsigned destination, unsigned source) noexcept
{
	return 0xaa00'03e0U | ((source & 31U) << 16) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t move_from_sp(
	unsigned destination) noexcept
{
	return 0x9100'0000U | (31U << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t move_to_sp(
	unsigned source) noexcept
{
	return 0x9100'0000U | ((source & 31U) << 5) | 31U;
}

void store_payload(
	std::vector<std::byte> &bytes, std::uint64_t address)
{
	const auto offset = static_cast<std::size_t>(
		address - pauth::data_base);
	pauth::require(offset <= bytes.size() - sizeof(payload));
	std::memcpy(bytes.data() + offset, &payload, sizeof(payload));
}

void check_success(
	test::EngineHarness &harness, std::string_view name,
	bool key_b, bool writeback, unsigned encoded_offset,
	unsigned base, unsigned destination, std::uint64_t plain_base)
{
	const auto displacement =
		static_cast<std::int64_t>(
			static_cast<std::int16_t>(
				(encoded_offset & 0x200U) != 0 ?
					encoded_offset | 0xfc00U :
					encoded_offset)) *
		8;
	const auto address = static_cast<std::uint64_t>(
		static_cast<std::int64_t>(plain_base) + displacement);
	const unsigned scratch = destination == 28U ? 27U : 28U;
	auto initial = pauth::initial_state(
		encoded_offset + base * 1024 + destination * 32768);
	initial.registers[scratch] = plain_base;

	std::array<test::ProgramWord, 10> code{};
	std::size_t count = 0;
	const auto emit = [&](std::uint32_t instruction) {
		code[count] = {
			.offset = count * sizeof(std::uint32_t),
			.instruction = instruction,
		};
		++count;
	};
	emit(pauth::data_processing(key_b ? 11U : 10U, scratch));
	if (base == 31U)
		emit(move_to_sp(scratch));
	else if (base != scratch)
		emit(move_register(base, scratch));
	emit(pauth::authenticated_load(
		key_b, writeback, encoded_offset, base, destination));

	if (base == 31U) {
		if (!writeback) {
			emit(move_from_sp(scratch));
			emit(pauth::xpac(true, scratch));
			emit(move_to_sp(scratch));
		} else {
			emit(pauth::xpac(true, scratch));
		}
	} else {
		if (!writeback && (base != destination || destination == 31U))
			emit(pauth::xpac(true, base));
		if (scratch != base)
			emit(pauth::xpac(true, scratch));
	}
	emit(0xd400'0001U);

	auto expected = initial;
	if (destination < 31U)
		expected.registers[destination] = payload;
	if (base == 31U) {
		expected.sp = writeback ? address : plain_base;
		expected.registers[scratch] = plain_base;
	} else if (writeback) {
		expected.registers[base] = address;
		expected.registers[scratch] =
			base == scratch ? address : plain_base;
	} else if (base != destination || destination == 31U) {
		expected.registers[base] = plain_base;
		expected.registers[scratch] = plain_base;
	}
	expected.pc =
		test::engine_test_code_base + count * sizeof(std::uint32_t);

	std::vector<std::byte> bytes(3 * page_size);
	store_payload(bytes, address);
	const std::array memory{
		test::MemoryRegion{
			.address = pauth::data_base,
			.initial = bytes,
			.expected = {},
			.readable = true,
			.writable = true,
		},
	};
	harness.check_program({
		.name = name,
		.code = std::span<const test::ProgramWord>{
			code.data(), count},
		.initial = initial,
		.expected = expected,
		.native = pauth::native_pauth,
		.memory = memory,
	});
}

void every_offset(
	test::EngineHarness &harness, std::size_t &cases)
{
	const std::uint64_t plain_base =
		pauth::data_base + page_size + page_size / 2;
	for (const bool key_b : {false, true}) {
		for (const bool writeback : {false, true}) {
			for (unsigned encoded_offset = 0;
			     encoded_offset < 1024; ++encoded_offset) {
				const pauth::CaseName name{
					"ldr%s-%s-offset%u",
					key_b ? "ab" : "aa",
					writeback ? "pre" : "offset",
					encoded_offset};
				check_success(
					harness, name, key_b, writeback,
					encoded_offset, 3, 7, plain_base);
				++cases;
			}
		}
	}
}

void every_register_field(
	test::EngineHarness &harness, std::size_t &cases)
{
	const std::uint64_t plain_base =
		pauth::data_base + page_size + page_size / 2;
	for (const bool key_b : {false, true}) {
		for (const bool writeback : {false, true}) {
			for (unsigned base = 0; base < 32; ++base) {
				for (unsigned destination = 0;
				     destination < 32; ++destination) {
					if (writeback && base == destination &&
					    base != 31U)
						continue;
					const pauth::CaseName name{
						"ldr%s-%s-n%u-t%u",
						key_b ? "ab" : "aa",
						writeback ? "pre" : "offset",
						base, destination};
					check_success(
						harness, name, key_b, writeback,
						0, base, destination, plain_base);
					++cases;
				}
			}
		}
	}
}

void cross_page_loads(
	test::EngineHarness &harness, std::size_t &cases)
{
	const std::uint64_t plain_base =
		pauth::data_base + 2 * page_size - 4;
	for (const bool key_b : {false, true}) {
		const pauth::CaseName name{
			"ldr%s-cross-page", key_b ? "ab" : "aa"};
		check_success(
			harness, name, key_b, false, 0, 3, 7,
			plain_base);
		++cases;
	}
}

void fault_atomicity(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (const bool writeback : {false, true}) {
			auto initial = pauth::initial_state(
				0x800 + static_cast<unsigned>(key_b) * 4 +
				static_cast<unsigned>(writeback));
			initial.registers[3] = 0x7a00'1000ULL;
			const pauth::CaseName name{
				"ldr%s-%s-unmapped",
				key_b ? "ab" : "aa",
				writeback ? "pre" : "offset"};
			harness.check({
				.name = name,
				.instruction = pauth::authenticated_load(
					key_b, writeback, 1, 3, 7),
				.initial = initial,
				.expected = initial,
				.event = test::ExpectedEvent::memory_fault,
				.native = pauth::native_pauth,
			});
			++cases;
		}
	}
}

void validate_signed_fault_state(
	const pauth::arm64::CpuState &initial,
	const pauth::arm64::CpuState &expected_template,
	const pauth::arm64::CpuState &actual,
	std::string_view case_name, std::string_view engine,
	std::uint32_t instruction)
{
	pauth::require(
		pauth::strip(actual.registers[signed_fault_base], true) ==
			initial.registers[signed_fault_base]);
	auto expected = initial;
	expected.pc = expected_template.pc;
	expected.registers[signed_fault_base] =
		actual.registers[signed_fault_base];
	test::require_architecture_equal(
		expected, actual, case_name, engine, instruction);
}

void signed_fault_address(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (const bool writeback : {false, true}) {
			auto initial = pauth::initial_state(
				0x880 + static_cast<unsigned>(key_b) * 4 +
				static_cast<unsigned>(writeback));
			initial.registers[signed_fault_base] =
				signed_fault_plain_base;
			const std::array code{
				test::ProgramWord{
					.offset = 0,
					.instruction = pauth::data_processing(
						key_b ? 11U : 10U,
						signed_fault_base),
				},
				test::ProgramWord{
					.offset = 4,
					.instruction =
						pauth::authenticated_load(
							key_b, writeback,
							signed_fault_encoded_offset,
							signed_fault_base,
							signed_fault_destination),
				},
			};
			auto expected = initial;
			expected.pc = test::engine_test_code_base + 4;
			const pauth::CaseName name{
				"ldr%s-%s-signed-unmapped",
				key_b ? "ab" : "aa",
				writeback ? "pre" : "offset"};
			harness.check_program({
				.name = name,
				.code = code,
				.initial = initial,
				.expected = expected,
				.event = test::ExpectedEvent::memory_fault,
				.native = pauth::native_pauth,
				.fault = {
					.address = signed_fault_plain_base + 8,
					.access =
						pauth::arm64::MemoryAccess::read,
				},
				.compare_fault = true,
				.validator = &validate_signed_fault_state,
			});
			++cases;
		}
	}
}

void sp_alignment_fault(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (const bool writeback : {false, true}) {
			auto initial = pauth::initial_state(
				0x900 + static_cast<unsigned>(key_b) * 4 +
				static_cast<unsigned>(writeback));
			initial.sp = pauth::data_base + page_size + 8;
			const pauth::CaseName name{
				"ldr%s-%s-sp-align",
				key_b ? "ab" : "aa",
				writeback ? "pre" : "offset"};
			harness.check({
				.name = name,
				.instruction = pauth::authenticated_load(
					key_b, writeback, 0, 31, 7),
				.initial = initial,
				.expected = initial,
				.event = test::ExpectedEvent::memory_fault,
				.native = pauth::native_pauth,
				.fault = {
					.address = initial.sp,
					.access =
						pauth::arm64::MemoryAccess::read,
				},
				.compare_fault = true,
			});
			++cases;
		}
	}
}

void writeback_overlap_is_undefined(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const bool key_b : {false, true}) {
		for (unsigned base = 0; base < 31; ++base) {
			for (unsigned encoded_offset = 0;
			     encoded_offset < 1024; ++encoded_offset) {
				const pauth::CaseName name{
					"ldr%s-overlap-n%u-o%u",
					key_b ? "ab" : "aa",
					base, encoded_offset};
				harness.check_illegal(
					name,
					pauth::authenticated_load(
						key_b, true, encoded_offset,
						base, base),
					pauth::initial_state(
						static_cast<unsigned>(key_b) *
							32768 +
						base * 1024 + encoded_offset),
					pauth::native_pauth);
				++cases;
			}
		}
	}
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool key_b : {false, true}) {
		for (const bool writeback : {false, true}) {
			for (const bool base_sp : {false, true}) {
				for (const bool discard : {false, true}) {
					const auto built =
						pauth::gadgets::Builder{}.build(
							pauth::pc,
							std::array{
								pauth::authenticated_load(
									key_b, writeback, 0,
									base_sp ? 31U : 3U,
									discard ? 31U : 7U)});
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
}

} // namespace

int main()
{
	test::EngineHarness harness;
	std::size_t cases = 0;
	every_offset(harness, cases);
	every_register_field(harness, cases);
	cross_page_loads(harness, cases);
	fault_atomicity(harness, cases);
	signed_fault_address(harness, cases);
	sp_alignment_fault(harness, cases);
	writeback_overlap_is_undefined(harness, cases);
	production_specialization();
	pauth::require_counts(harness, cases);
	harness.print_summary("arm64-base-pauth-load");
	return 0;
}
