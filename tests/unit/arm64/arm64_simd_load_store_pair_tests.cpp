// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_pair_test_support.hpp"

#include <algorithm>
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

struct Variant final {
	unsigned operation;
	std::size_t size;
	bool load;
	std::string_view name;
};

constexpr std::array variants{
	Variant{0, 4, false, "vector-store-32"},
	Variant{0, 4, true, "vector-load-32"},
	Variant{1, 8, false, "vector-store-64"},
	Variant{1, 8, true, "vector-load-64"},
	Variant{2, 16, false, "vector-store-128"},
	Variant{2, 16, true, "vector-load-128"},
};

[[nodiscard]] engine_tests::NativeRequirements native_requirement()
{
	return engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::floating_point);
}

[[nodiscard]] constexpr bool writeback(unsigned mode)
{
	return mode == 1U || mode == 3U;
}

[[nodiscard]] std::uint64_t effective_address(
	std::uint64_t base, const Variant &variant, unsigned mode, unsigned imm7)
{
	const auto adjusted = base + static_cast<std::uint64_t>(
		pair_test::signed_imm7(imm7) *
		static_cast<std::int64_t>(variant.size));
	return mode == 1U ? base : adjusted;
}

void load_vector(
	arm64::VectorRegister &destination, std::span<const std::byte> memory,
	std::size_t offset, std::size_t size)
{
	destination.bytes.fill(std::byte{});
	std::copy_n(memory.begin() + static_cast<std::ptrdiff_t>(offset),
		size, destination.bytes.begin());
}

void store_vector(
	std::span<std::byte> memory, std::size_t offset,
	const arm64::VectorRegister &source, std::size_t size)
{
	std::copy_n(source.bytes.begin(), size,
		memory.begin() + static_cast<std::ptrdiff_t>(offset));
}

void exhaustive_allocated_matrix(engine_tests::EngineHarness &harness)
{
	const auto base = pair_test::data_base + pair_test::page_size / 2U;
	for (unsigned variant_index = 0; variant_index < variants.size();
	     ++variant_index) {
		const auto &variant = variants[variant_index];
		for (unsigned mode = 0; mode < 4; ++mode)
			for (unsigned imm7 = 0; imm7 < 128; ++imm7) {
				auto memory = std::vector<std::byte>(pair_test::page_size);
				pair_test::fill_memory(memory,
					variant_index * 19U + mode * 7U + imm7);
				auto expected_memory = memory;
				auto initial = pair_test::seeded_state(
					0x9100'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant_index) << 24) |
					(mode << 16) | imm7);
				initial.registers[base_register] = base;
				auto expected = initial;
				const auto address =
					effective_address(base, variant, mode, imm7);
				const auto offset =
					static_cast<std::size_t>(address - pair_test::data_base);
				if (variant.load) {
					load_vector(expected.vectors[first_register], memory,
						offset, variant.size);
					load_vector(expected.vectors[second_register], memory,
						offset + variant.size, variant.size);
				} else {
					store_vector(expected_memory, offset,
						initial.vectors[first_register], variant.size);
					store_vector(expected_memory, offset + variant.size,
						initial.vectors[second_register], variant.size);
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
					pair_test::pair(variant.operation, true, variant.load,
						mode, imm7, base_register, first_register,
						second_register),
					initial, expected, native_requirement(), memory,
					variant.load ?
						std::span<const std::byte>{} :
						std::span<const std::byte>{expected_memory});
			}
	}
}

void duplicate_load_destinations(engine_tests::EngineHarness &harness)
{
	auto initial = pair_test::seeded_state(0x9300'0000'0000'0000ULL);
	initial.registers[base_register] =
		pair_test::data_base + pair_test::page_size / 2U;
	for (const auto &variant : variants) {
		if (!variant.load)
			continue;
		for (unsigned mode = 0; mode < 4; ++mode)
			for (unsigned destination = 0; destination < 32; ++destination) {
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s duplicate destination=%u mode=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(), destination, mode);
				harness.check_illegal(name,
					pair_test::pair(variant.operation, true, true, mode,
						0, base_register, destination, destination),
					initial, native_requirement());
			}
	}
}

void stack_and_vector_31_mappings(engine_tests::EngineHarness &harness)
{
	const auto base = pair_test::data_base + pair_test::page_size / 2U;
	for (unsigned variant_index = 0; variant_index < variants.size();
	     ++variant_index) {
		const auto &variant = variants[variant_index];
		for (unsigned mode = 0; mode < 4; ++mode) {
			for (unsigned register_31_position = 0;
			     register_31_position < 2; ++register_31_position) {
				auto memory = std::vector<std::byte>(pair_test::page_size);
				pair_test::fill_memory(memory,
					0x30U + variant_index * 11U + mode * 3U +
						register_31_position);
				auto expected_memory = memory;
				auto initial = pair_test::seeded_state(
					0x9400'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant_index) << 20) |
					(mode << 12) | register_31_position);
				initial.sp = base;
				auto expected = initial;
				const unsigned first =
					register_31_position == 0U ? 31U : 30U;
				const unsigned second =
					register_31_position == 0U ? 30U : 31U;
				constexpr unsigned imm7 = 3;
				const auto address =
					effective_address(base, variant, mode, imm7);
				const auto offset =
					static_cast<std::size_t>(address - pair_test::data_base);
				if (variant.load) {
					load_vector(expected.vectors[first], memory,
						offset, variant.size);
					load_vector(expected.vectors[second], memory,
						offset + variant.size, variant.size);
				} else {
					store_vector(expected_memory, offset,
						initial.vectors[first], variant.size);
					store_vector(expected_memory, offset + variant.size,
						initial.vectors[second], variant.size);
				}
				if (writeback(mode))
					expected.sp = base + 3U * variant.size;
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s sp-base v31-position=%u mode=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(), register_31_position, mode);
				pair_test::check_memory(
					harness, name,
					pair_test::pair(variant.operation, true, variant.load,
						mode, imm7, 31, first, second),
					initial, expected, native_requirement(), memory,
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
				0x9500'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(variant_index) << 16) | mode);
			initial.sp = base;
			auto expected = initial;
			constexpr unsigned imm7 = 1;
			const auto address =
				effective_address(base, variant, mode, imm7);
			const auto offset =
				static_cast<std::size_t>(address - pair_test::data_base);
			store_vector(expected_memory, offset, initial.vectors[31],
				variant.size);
			store_vector(expected_memory, offset + variant.size,
				initial.vectors[31], variant.size);
			if (writeback(mode))
				expected.sp = base + variant.size;
			char name[112];
			std::snprintf(name, sizeof(name),
				"%.*s sp-base duplicate-v31-store mode=%u",
				static_cast<int>(variant.name.size()), variant.name.data(),
				mode);
			pair_test::check_memory(
				harness, name,
				pair_test::pair(variant.operation, true, false, mode,
					imm7, 31, 31, 31),
				initial, expected, native_requirement(), memory,
				expected_memory);
		}
	}
}

void same_register_number_is_not_overlap(
	engine_tests::EngineHarness &harness)
{
	const auto base = pair_test::data_base + pair_test::page_size / 2U;
	for (unsigned variant_index = 0; variant_index < variants.size();
	     ++variant_index) {
		const auto &variant = variants[variant_index];
		for (unsigned mode = 0; mode < 4; ++mode) {
			auto memory = std::vector<std::byte>(pair_test::page_size);
			pair_test::fill_memory(memory,
				0xa0U + variant_index * 5U + mode);
			auto expected_memory = memory;
			auto initial = pair_test::seeded_state(
				0x9600'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(variant_index) << 16) | mode);
			initial.registers[5] = base;
			auto expected = initial;
			constexpr unsigned imm7 = 2;
			const auto address =
				effective_address(base, variant, mode, imm7);
			const auto offset =
				static_cast<std::size_t>(address - pair_test::data_base);
			if (variant.load) {
				load_vector(expected.vectors[5], memory,
					offset, variant.size);
				load_vector(expected.vectors[6], memory,
					offset + variant.size, variant.size);
			} else {
				store_vector(expected_memory, offset,
					initial.vectors[5], variant.size);
				store_vector(expected_memory, offset + variant.size,
					initial.vectors[6], variant.size);
			}
			if (writeback(mode))
				expected.registers[5] = base + 2U * variant.size;
			char name[112];
			std::snprintf(name, sizeof(name),
				"%.*s same-gpr-vector-number mode=%u",
				static_cast<int>(variant.name.size()), variant.name.data(),
				mode);
			pair_test::check_memory(
				harness, name,
				pair_test::pair(variant.operation, true, variant.load,
					mode, imm7, 5, 5, 6),
				initial, expected, native_requirement(), memory,
				variant.load ?
					std::span<const std::byte>{} :
					std::span<const std::byte>{expected_memory});
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
		for (unsigned mode = 0; mode < 4; ++mode)
			for (unsigned split = 0; split < 2; ++split) {
				const auto target = boundary -
					(split == 0U ? variant.size : variant.size / 2U);
				constexpr unsigned imm7 = 1;
				const auto base =
					base_for_target(target, variant, mode, imm7);
				auto memory =
					std::vector<std::byte>(2U * pair_test::page_size);
				pair_test::fill_memory(memory,
					0xc0U + variant_index * 9U + mode * 2U + split);
				auto expected_memory = memory;
				auto initial = pair_test::seeded_state(
					0x9700'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant_index) << 20) |
					(mode << 12) | split);
				initial.registers[base_register] = base;
				auto expected = initial;
				const auto offset =
					static_cast<std::size_t>(target - pair_test::data_base);
				if (variant.load) {
					load_vector(expected.vectors[first_register], memory,
						offset, variant.size);
					load_vector(expected.vectors[second_register], memory,
						offset + variant.size, variant.size);
				} else {
					store_vector(expected_memory, offset,
						initial.vectors[first_register], variant.size);
					store_vector(expected_memory, offset + variant.size,
						initial.vectors[second_register], variant.size);
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
					pair_test::pair(variant.operation, true, variant.load,
						mode, imm7, base_register, first_register,
						second_register),
					initial, expected, native_requirement(), memory,
					variant.load ?
						std::span<const std::byte>{} :
						std::span<const std::byte>{expected_memory});
			}
	}
}

void cross_page_fault_atomicity(engine_tests::EngineHarness &harness)
{
	const auto boundary = pair_test::data_base + pair_test::page_size;
	for (unsigned variant_index = 0; variant_index < variants.size();
	     ++variant_index) {
		const auto &variant = variants[variant_index];
		for (unsigned mode = 0; mode < 4; ++mode)
			for (unsigned split = 0; split < 2; ++split) {
				const auto target = boundary -
					(split == 0U ? variant.size : variant.size / 2U);
				constexpr unsigned imm7 = 1;
				const auto base =
					base_for_target(target, variant, mode, imm7);
				auto memory = std::vector<std::byte>(pair_test::page_size);
				pair_test::fill_memory(memory,
					0xe0U + variant_index * 7U + mode * 2U + split);
				auto initial = pair_test::seeded_state(
					0x9800'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant_index) << 20) |
					(mode << 12) | split);
				initial.registers[base_register] = base;
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s fault-atomic split=%u mode=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(), split, mode);
				pair_test::check_memory(
					harness, name,
					pair_test::pair(variant.operation, true, variant.load,
						mode, imm7, base_register, first_register,
						second_register),
					initial, initial, native_requirement(), memory, {},
					engine_tests::ExpectedEvent::memory_fault,
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

void stack_alignment_faults(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0; variant_index < variants.size();
	     ++variant_index) {
		const auto &variant = variants[variant_index];
		for (unsigned mode = 0; mode < 4; ++mode) {
			auto memory = std::vector<std::byte>(pair_test::page_size);
			pair_test::fill_memory(memory,
				0x20U + variant_index * 5U + mode);
			auto initial = pair_test::seeded_state(
				0x9900'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(variant_index) << 16) | mode);
			initial.sp = pair_test::data_base + pair_test::page_size / 2U + 8U;
			char name[112];
			std::snprintf(name, sizeof(name),
				"%.*s misaligned-sp mode=%u",
				static_cast<int>(variant.name.size()), variant.name.data(),
				mode);
			pair_test::check_memory(
				harness, name,
				pair_test::pair(variant.operation, true, variant.load,
					mode, 3, 31, first_register, second_register),
				initial, initial, native_requirement(), memory, {},
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
	duplicate_load_destinations(harness);
	stack_and_vector_31_mappings(harness);
	same_register_number_is_not_overlap(harness);
	cross_page_success(harness);
	cross_page_fault_atomicity(harness);
	stack_alignment_faults(harness);
	harness.print_summary("arm64-simd-load-store-pair");
}
