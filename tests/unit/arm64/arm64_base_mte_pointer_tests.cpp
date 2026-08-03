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
	mte::require(mte::gmi(0, 1, 2) == 0x9ac2'1420U);
	mte::require(mte::gmi(30, 31, 31) == 0x9adf'17feU);
	mte::require(mte::subp(false, 0, 1, 2) == 0x9ac2'0020U);
	mte::require(mte::subp(false, 30, 31, 31) == 0x9adf'03feU);
	mte::require(mte::subp(true, 0, 1, 2) == 0xbac2'0020U);
	mte::require(mte::subp(true, 31, 1, 2) == 0xbac2'003fU);
	mte::require(mte::subp(true, 31, 31, 31) == 0xbadf'03ffU);
}

void all_register_fields(test::EngineHarness &harness)
{
	for (unsigned destination = 0; destination < 32; ++destination) {
		for (unsigned first = 0; first < 32; ++first) {
			for (unsigned second = 0; second < 32; ++second) {
				auto initial = mte::initial_state();
				initial.sp = 0xaf80'0000'0000'0040ULL;
				const mte::CaseName gmi_name{
					"gmi-d%u-n%u-m%u", destination, first, second};
				mte::check_one(harness, gmi_name,
					mte::gmi(destination, first, second), initial,
					mte::expected_gmi(
						initial, destination, first, second));
				for (const bool flags : {false, true}) {
				const mte::CaseName name{
					"%s%s-d%u-n%u-m%u",
					flags && destination == 31U ?
						"BASE-0098 CMPP " : "",
					flags ? "subps" : "subp",
						destination, first, second};
					mte::check_one(harness, name,
						mte::subp(flags, destination, first, second),
						initial, mte::expected_subp(initial, flags,
							destination, first, second));
				}
			}
		}
	}
}

void tag_mask_and_pointer_edges(test::EngineHarness &harness)
{
	constexpr std::array masks{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0xffff},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL}};
	for (unsigned tag = 0; tag < 16; ++tag) {
		for (const auto mask : masks) {
			auto initial = mte::initial_state();
			initial.registers[1] =
				(static_cast<std::uint64_t>(tag) << 56) |
				0x00ab'1234'5678'9abcULL;
			initial.registers[2] = mask;
			const mte::CaseName name{"gmi-tag%u-mask%llx", tag,
				static_cast<unsigned long long>(mask)};
			mte::check_one(harness, name, mte::gmi(3, 1, 2), initial,
				mte::expected_gmi(initial, 3, 1, 2));
		}
	}

	constexpr std::array values{
		std::uint64_t{0}, std::uint64_t{1},
		std::uint64_t{0x007f'ffff'ffff'ffffULL},
		std::uint64_t{0x0080'0000'0000'0000ULL},
		std::uint64_t{0x00ff'ffff'ffff'ffffULL},
		std::uint64_t{0xab00'0000'0000'0001ULL},
		std::uint64_t{0xcd7f'ffff'ffff'ffffULL},
		std::uint64_t{0xef80'0000'0000'0000ULL}};
	for (unsigned first = 0; first < values.size(); ++first) {
		for (unsigned second = 0; second < values.size(); ++second) {
			for (const bool flags : {false, true}) {
				auto initial = mte::initial_state();
				initial.registers[1] = values[first];
				initial.registers[2] = values[second];
				const mte::CaseName name{"%s-edge%u-%u",
					flags ? "subps" : "subp", first, second};
				mte::check_one(harness, name,
					mte::subp(flags, 3, 1, 2), initial,
					mte::expected_subp(initial, flags, 3, 1, 2));
			}
		}
	}
}

void reserved_allocations(test::EngineHarness &harness)
{
	const auto initial = mte::initial_state();
	/*
	 * These are explicit UNALLOCATED cells in the data-processing
	 * (two-source) allocation: sf==0/opcode==000000, opcode==000001,
	 * opcode==001101, and S==1 with a non-SUBPS opcode.
	 */
	constexpr std::array invalid{
		0x1ac0'0020U,
		0x9ac0'0420U,
		0x9ac0'3820U,
		0xbac0'1020U,
		0xbac0'1420U,
		0x9ac0'3420U,
	};
	for (unsigned index = 0; index < invalid.size(); ++index) {
		const mte::CaseName name{"mte-pointer-reserved-%u", index};
		harness.check_illegal(name, invalid[index], initial, mte::native_mte);
	}
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (unsigned operation = 0; operation < 3; ++operation) {
		for (unsigned mode = 0; mode < 8; ++mode) {
			const unsigned first = (mode & 4U) != 0 ? 31U : 1U;
			const unsigned second = (mode & 2U) != 0 ? 31U : 2U;
			const unsigned destination = (mode & 1U) != 0 ? 31U : 3U;
			const auto instruction = operation == 0 ?
				mte::gmi(destination, first, second) :
				mte::subp(operation == 2, destination, first, second);
			const auto built = gadgets::Builder{}.build(
				test::engine_test_code_base, std::array{instruction});
			mte::require(built.complete &&
				built.program.instruction_count() == 1 &&
				built.program.words().size() == 4);
			entries.insert(built.program.words().front());
		}
	}
	mte::require(entries.size() == 24);
}

} // namespace

int main()
{
	encoding_examples();
	test::EngineHarness harness;
	all_register_fields(harness);
	tag_mask_and_pointer_edges(harness);
	reserved_allocations(harness);
	production_specialization();
	harness.print_summary("arm64-base-mte-pointer");
}
