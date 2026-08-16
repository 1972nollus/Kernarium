// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_pair_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;
namespace pair_test = arm64_load_store_pair_test;

namespace {

constexpr unsigned base_register = 28;
constexpr unsigned first_register = 3;
constexpr unsigned second_register = 17;
constexpr std::uint64_t first_store = 0xf123'4567'89ab'cdefULL;
constexpr std::uint64_t second_store = 0x8123'4567'0bad'f00dULL;

struct Variant final {
	unsigned operation;
	std::size_t size;
	bool load;
	std::string_view name;
};

constexpr std::array variants{
	Variant{0, 4, false, "store-32"},
	Variant{0, 4, true, "load-32"},
	Variant{1, 4, true, "load-signed-32"},
	Variant{2, 8, false, "store-64"},
	Variant{2, 8, true, "load-64"},
};

[[nodiscard]] constexpr bool valid_mode(
	const Variant &variant, unsigned mode)
{
	return variant.operation != 1U || mode != 0U;
}

[[nodiscard]] constexpr bool writeback(unsigned mode)
{
	return mode == 1U || mode == 3U;
}

[[nodiscard]] constexpr std::uint64_t sign_extend_word(std::uint64_t value)
{
	return static_cast<std::uint64_t>(
		static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

[[nodiscard]] constexpr std::uint64_t loaded_value(
	const Variant &variant, std::uint64_t value)
{
	if (variant.operation == 1U)
		return sign_extend_word(value);
	return variant.size == 4U ? value & 0xffff'ffffU : value;
}

[[nodiscard]] std::uint64_t effective_address(
	std::uint64_t base, const Variant &variant, unsigned mode, unsigned imm7)
{
	const auto adjusted = base + static_cast<std::uint64_t>(
		pair_test::signed_imm7(imm7) *
		static_cast<std::int64_t>(variant.size));
	return mode == 1U ? base : adjusted;
}

void exhaustive_allocated_matrix(engine_tests::EngineHarness &harness)
{
	const auto base = pair_test::data_base + pair_test::page_size / 2U;
	for (unsigned variant_index = 0; variant_index < variants.size();
	     ++variant_index) {
		const auto &variant = variants[variant_index];
		for (unsigned mode = 0; mode < 4; ++mode) {
			if (!valid_mode(variant, mode))
				continue;
			for (unsigned imm7 = 0; imm7 < 128; ++imm7) {
				auto memory = std::vector<std::byte>(pair_test::page_size);
				pair_test::fill_memory(memory,
					variant_index * 19U + mode * 7U + imm7);
				auto expected_memory = memory;
				auto initial = pair_test::seeded_state(
					0x1000'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant_index) << 24) |
					(mode << 16) | imm7);
				initial.registers[base_register] = base;
				initial.registers[first_register] = first_store;
				initial.registers[second_register] = second_store;
				auto expected = initial;
				const auto address =
					effective_address(base, variant, mode, imm7);
				const auto offset =
					static_cast<std::size_t>(address - pair_test::data_base);
				if (variant.load) {
					expected.registers[first_register] = loaded_value(
						variant, pair_test::decode_little_endian(
							memory, offset, variant.size));
					expected.registers[second_register] = loaded_value(
						variant, pair_test::decode_little_endian(
							memory, offset + variant.size, variant.size));
				} else {
					pair_test::encode_little_endian(expected_memory,
						offset, variant.size, first_store);
					pair_test::encode_little_endian(expected_memory,
						offset + variant.size, variant.size, second_store);
				}
				if (writeback(mode))
					expected.registers[base_register] =
						base + static_cast<std::uint64_t>(
							pair_test::signed_imm7(imm7) *
							static_cast<std::int64_t>(variant.size));
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s mode=%u imm7=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(), mode, imm7);
				pair_test::check_memory(
					harness, name,
					pair_test::pair(variant.operation, false, variant.load,
						mode, imm7, base_register, first_register,
						second_register),
					initial, expected,
					engine_tests::NativeRequirements::baseline(), memory,
					variant.load ?
						std::span<const std::byte>{} :
						std::span<const std::byte>{expected_memory});
			}
		}
	}
}

void reserved_allocations(engine_tests::EngineHarness &harness)
{
	const auto initial = pair_test::seeded_state(0x2000'0000'0000'0000ULL);
	/*
	 * In the ordinary GPR pair family, opc=01 has only the three LDPSW
	 * addressing modes. The opc=01 stores in modes 1..3 belong to the
	 * separately feature-gated tag-pair family.
	 *
	 * GPR opc=11 is the FEAT_LSUI unprivileged pair family. Its exhaustive
	 * allocation and behavior coverage lives in the split LSUI pair suites.
	 */
	for (const bool load : {false, true})
		for (unsigned imm7 = 0; imm7 < 128; ++imm7) {
			char name[96];
			std::snprintf(name, sizeof(name),
				"reserved operation=1 load=%u non-temporal imm7=%u",
				static_cast<unsigned>(load), imm7);
			harness.check_illegal(name,
				pair_test::pair(1, false, load, 0, imm7,
					base_register, first_register, second_register),
				initial);
		}
}

void duplicate_load_destinations(engine_tests::EngineHarness &harness)
{
	auto initial = pair_test::seeded_state(0x3000'0000'0000'0000ULL);
	initial.registers[base_register] =
		pair_test::data_base + pair_test::page_size / 2U;
	for (const auto &variant : variants) {
		if (!variant.load)
			continue;
		for (unsigned mode = 0; mode < 4; ++mode) {
			if (!valid_mode(variant, mode))
				continue;
			for (unsigned destination = 0; destination < 32; ++destination) {
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s duplicate destination=%u mode=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(), destination, mode);
				harness.check_illegal(name,
					pair_test::pair(variant.operation, false, true, mode,
						0, base_register, destination, destination),
					initial);
			}
		}
	}
}

void stack_and_zero_register_mappings(engine_tests::EngineHarness &harness)
{
	const auto base = pair_test::data_base + pair_test::page_size / 2U;
	for (unsigned variant_index = 0; variant_index < variants.size();
	     ++variant_index) {
		const auto &variant = variants[variant_index];
		for (unsigned mode = 0; mode < 4; ++mode) {
			if (!valid_mode(variant, mode))
				continue;
			for (unsigned zero_position = 0; zero_position < 2;
			     ++zero_position) {
				auto memory = std::vector<std::byte>(pair_test::page_size);
				pair_test::fill_memory(memory,
					0x40U + variant_index * 11U + mode * 3U +
						zero_position);
				auto expected_memory = memory;
				auto initial = pair_test::seeded_state(
					0x4000'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant_index) << 20) |
					(mode << 12) | zero_position);
				initial.sp = base;
				initial.registers[30] = first_store;
				auto expected = initial;
				const unsigned first =
					zero_position == 0U ? 31U : 30U;
				const unsigned second =
					zero_position == 0U ? 30U : 31U;
				constexpr unsigned imm7 = 3;
				const auto address =
					effective_address(base, variant, mode, imm7);
				const auto offset =
					static_cast<std::size_t>(address - pair_test::data_base);
				if (variant.load) {
					const auto destination_offset =
						zero_position == 0U ?
							offset + variant.size : offset;
					expected.registers[30] = loaded_value(
						variant, pair_test::decode_little_endian(
							memory, destination_offset, variant.size));
				} else {
					pair_test::encode_little_endian(expected_memory,
						offset, variant.size,
						first == 31U ? 0U : first_store);
					pair_test::encode_little_endian(expected_memory,
						offset + variant.size, variant.size,
						second == 31U ? 0U : first_store);
				}
				if (writeback(mode))
					expected.sp = base +
						static_cast<std::uint64_t>(
							pair_test::signed_imm7(imm7) *
							static_cast<std::int64_t>(variant.size));
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s sp-base zero-position=%u mode=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(), zero_position, mode);
				pair_test::check_memory(
					harness, name,
					pair_test::pair(variant.operation, false, variant.load,
						mode, imm7, 31, first, second),
					initial, expected,
					engine_tests::NativeRequirements::baseline(), memory,
					variant.load ?
						std::span<const std::byte>{} :
						std::span<const std::byte>{expected_memory});
			}

			if (variant.load)
				continue;
			auto memory = std::vector<std::byte>(pair_test::page_size);
			pair_test::fill_memory(memory,
				0x70U + variant_index * 7U + mode);
			auto expected_memory = memory;
			auto initial = pair_test::seeded_state(
				0x4100'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(variant_index) << 16) | mode);
			initial.sp = base;
			auto expected = initial;
			constexpr unsigned imm7 = 1;
			const auto address =
				effective_address(base, variant, mode, imm7);
			const auto offset =
				static_cast<std::size_t>(address - pair_test::data_base);
			pair_test::encode_little_endian(
				expected_memory, offset, variant.size, 0);
			pair_test::encode_little_endian(
				expected_memory, offset + variant.size, variant.size, 0);
			if (writeback(mode))
				expected.sp = base + variant.size;
			char name[112];
			std::snprintf(name, sizeof(name),
				"%.*s sp-base both-zero mode=%u",
				static_cast<int>(variant.name.size()), variant.name.data(),
				mode);
			pair_test::check_memory(
				harness, name,
				pair_test::pair(variant.operation, false, false, mode,
					imm7, 31, 31, 31),
				initial, expected,
				engine_tests::NativeRequirements::baseline(), memory,
				expected_memory);
		}
	}
}

void base_transfer_overlap_choices(engine_tests::EngineHarness &harness)
{
	const auto base = pair_test::data_base + pair_test::page_size / 2U;
	for (unsigned variant_index = 0; variant_index < variants.size();
	     ++variant_index) {
		const auto &variant = variants[variant_index];
		for (unsigned mode = 0; mode < 4; ++mode) {
			if (!valid_mode(variant, mode))
				continue;
			for (unsigned overlap = 0; overlap < 2; ++overlap) {
				auto memory = std::vector<std::byte>(pair_test::page_size);
				pair_test::fill_memory(memory,
					0x90U + variant_index * 13U + mode * 2U + overlap);
				auto expected_memory = memory;
				auto initial = pair_test::seeded_state(
					0x5000'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant_index) << 20) |
					(mode << 12) | overlap);
				const unsigned first = overlap == 0U ? 5U : 6U;
				const unsigned second = overlap == 0U ? 6U : 5U;
				const unsigned base_reg = 5U;
				initial.registers[base_reg] = base;
				if (overlap == 0U)
					initial.registers[second] = second_store;
				else
					initial.registers[first] = first_store;
				auto expected = initial;
				constexpr unsigned imm7 = 2;
				const auto address =
					effective_address(base, variant, mode, imm7);
				const auto offset =
					static_cast<std::size_t>(address - pair_test::data_base);
				if (variant.load) {
					expected.registers[first] = loaded_value(
						variant, pair_test::decode_little_endian(
							memory, offset, variant.size));
					expected.registers[second] = loaded_value(
						variant, pair_test::decode_little_endian(
							memory, offset + variant.size, variant.size));
					/* This implementation selects WBSUPPRESS. */
				} else {
					pair_test::encode_little_endian(expected_memory,
						offset, variant.size,
						overlap == 0U ? base : first_store);
					pair_test::encode_little_endian(expected_memory,
						offset + variant.size, variant.size,
						overlap == 0U ? second_store : base);
					if (writeback(mode))
						expected.registers[base_reg] =
							base + 2U * variant.size;
				}
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s base-overlap=%u mode=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(), overlap, mode);
				pair_test::check_memory(
					harness, name,
					pair_test::pair(variant.operation, false, variant.load,
						mode, imm7, base_reg, first, second),
					initial, expected,
					engine_tests::NativeRequirements::baseline(), memory,
					variant.load ?
						std::span<const std::byte>{} :
						std::span<const std::byte>{expected_memory});
			}
		}
	}
}

[[nodiscard]] std::uint64_t base_for_target(
	std::uint64_t target, const Variant &variant, unsigned mode,
	unsigned imm7)
{
	if (mode == 1U)
		return target;
	return target - static_cast<std::uint64_t>(
		pair_test::signed_imm7(imm7) *
		static_cast<std::int64_t>(variant.size));
}

void cross_page_success(engine_tests::EngineHarness &harness)
{
	const auto boundary = pair_test::data_base + pair_test::page_size;
	for (unsigned variant_index = 0; variant_index < variants.size();
	     ++variant_index) {
		const auto &variant = variants[variant_index];
		for (unsigned mode = 0; mode < 4; ++mode) {
			if (!valid_mode(variant, mode))
				continue;
			for (unsigned split = 0; split < 2; ++split) {
				const auto target = boundary -
					(split == 0U ? variant.size : variant.size / 2U);
				constexpr unsigned imm7 = 1;
				const auto base =
					base_for_target(target, variant, mode, imm7);
				auto memory =
					std::vector<std::byte>(2U * pair_test::page_size);
				pair_test::fill_memory(memory,
					0xb0U + variant_index * 9U + mode * 2U + split);
				auto expected_memory = memory;
				auto initial = pair_test::seeded_state(
					0x6000'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant_index) << 20) |
					(mode << 12) | split);
				initial.registers[base_register] = base;
				initial.registers[first_register] = first_store;
				initial.registers[second_register] = second_store;
				auto expected = initial;
				const auto offset =
					static_cast<std::size_t>(target - pair_test::data_base);
				if (variant.load) {
					expected.registers[first_register] = loaded_value(
						variant, pair_test::decode_little_endian(
							memory, offset, variant.size));
					expected.registers[second_register] = loaded_value(
						variant, pair_test::decode_little_endian(
							memory, offset + variant.size, variant.size));
				} else {
					pair_test::encode_little_endian(expected_memory,
						offset, variant.size, first_store);
					pair_test::encode_little_endian(expected_memory,
						offset + variant.size, variant.size, second_store);
				}
				if (writeback(mode))
					expected.registers[base_register] =
						base + variant.size;
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s cross-page split=%u mode=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(), split, mode);
				pair_test::check_memory(
					harness, name,
					pair_test::pair(variant.operation, false, variant.load,
						mode, imm7, base_register, first_register,
						second_register),
					initial, expected,
					engine_tests::NativeRequirements::baseline(), memory,
					variant.load ?
						std::span<const std::byte>{} :
						std::span<const std::byte>{expected_memory});
			}
		}
	}
}

void cross_page_fault_atomicity(engine_tests::EngineHarness &harness)
{
	const auto boundary = pair_test::data_base + pair_test::page_size;
	for (unsigned variant_index = 0; variant_index < variants.size();
	     ++variant_index) {
		const auto &variant = variants[variant_index];
		for (unsigned mode = 0; mode < 4; ++mode) {
			if (!valid_mode(variant, mode))
				continue;
			for (unsigned split = 0; split < 2; ++split) {
				const auto target = boundary -
					(split == 0U ? variant.size : variant.size / 2U);
				constexpr unsigned imm7 = 1;
				const auto base =
					base_for_target(target, variant, mode, imm7);
				auto memory = std::vector<std::byte>(pair_test::page_size);
				pair_test::fill_memory(memory,
					0xd0U + variant_index * 7U + mode * 2U + split);
				auto initial = pair_test::seeded_state(
					0x7000'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant_index) << 20) |
					(mode << 12) | split);
				initial.registers[base_register] = base;
				initial.registers[first_register] = first_store;
				initial.registers[second_register] = second_store;
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s fault-atomic split=%u mode=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(), split, mode);
				pair_test::check_memory(
					harness, name,
					pair_test::pair(variant.operation, false, variant.load,
						mode, imm7, base_register, first_register,
						second_register),
					initial, initial,
					engine_tests::NativeRequirements::baseline(), memory,
					{}, engine_tests::ExpectedEvent::memory_fault,
					{
						.address = boundary,
						.access = variant.load ?
							arm64::MemoryAccess::read :
							arm64::MemoryAccess::write,
					},
					true);
			}
		}
	}
}

/* Every other fault case above addresses through a general register. A
 * frame-saving prologue instead stores through SP itself, which is the shape a
 * guest hits when its stack has grown into an unmapped guard page, so keep the
 * SP-based encoding pinned to the same precise write fault. */
void stack_pointer_base_fault_is_precise(engine_tests::EngineHarness &harness)
{
	constexpr auto instruction =
		pair_test::pair(2, false, false, 2, 4, 31, 19, 20);
	static_assert(instruction == 0xa902'53f3U); // stp x19, x20, [sp, #32]
	const auto boundary = pair_test::data_base + pair_test::page_size;
	auto memory = std::vector<std::byte>(pair_test::page_size);
	pair_test::fill_memory(memory, 0xe7U);
	auto initial = pair_test::seeded_state(0x6000'0000'0000'0000ULL);
	initial.sp = boundary - 32U;
	pair_test::check_memory(
		harness, "stp sp-relative fault", instruction,
		initial, initial, engine_tests::NativeRequirements::baseline(), memory,
		{}, engine_tests::ExpectedEvent::memory_fault,
		{
			.address = boundary,
			.access = arm64::MemoryAccess::write,
		},
		true);
}

void stack_alignment_faults(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0; variant_index < variants.size();
	     ++variant_index) {
		const auto &variant = variants[variant_index];
		for (unsigned mode = 0; mode < 4; ++mode) {
			if (!valid_mode(variant, mode))
				continue;
			auto memory = std::vector<std::byte>(pair_test::page_size);
			pair_test::fill_memory(memory,
				0xf0U + variant_index * 5U + mode);
			auto initial = pair_test::seeded_state(
				0x8000'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(variant_index) << 16) | mode);
			initial.sp = pair_test::data_base + pair_test::page_size / 2U + 8U;
			char name[112];
			std::snprintf(name, sizeof(name),
				"%.*s misaligned-sp mode=%u",
				static_cast<int>(variant.name.size()), variant.name.data(),
				mode);
			pair_test::check_memory(
				harness, name,
				pair_test::pair(variant.operation, false, variant.load,
					mode, 3, 31, first_register, second_register),
				initial, initial,
				engine_tests::NativeRequirements::baseline(), memory, {},
				engine_tests::ExpectedEvent::memory_fault,
				{
					.address = initial.sp,
					.access = variant.load ?
						arm64::MemoryAccess::read :
						arm64::MemoryAccess::write,
				},
				true);
		}
	}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	exhaustive_allocated_matrix(harness);
	reserved_allocations(harness);
	duplicate_load_destinations(harness);
	stack_and_zero_register_mappings(harness);
	base_transfer_overlap_choices(harness);
	cross_page_success(harness);
	cross_page_fault_atomicity(harness);
	stack_pointer_base_fault_is_precise(harness);
	stack_alignment_faults(harness);
	harness.print_summary("arm64-base-load-store-pair");
}
