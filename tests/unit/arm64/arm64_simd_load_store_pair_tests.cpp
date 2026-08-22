// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_pair_test_support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

/*
 * Android userspace runs with top-byte-ignore, so a guest vector pair leaves
 * the direct fast path for the tagged helper on every transfer. That helper
 * moves the whole pair through one contiguous buffer, which only happens to
 * match a q0/q1 register split for the 128-bit variant. The 32-bit and 64-bit
 * variants must split and pack the two elements exactly like the cross-page
 * path, so keep every allocated addressing mode pinned under TBI.
 */
void top_byte_ignore_transfers(engine_tests::EngineHarness &harness)
{
	constexpr std::uint64_t top_byte = 0x4200'0000'0000'0000ULL;
	constexpr unsigned imm7 = 2;
	const auto base = pair_test::data_base + pair_test::page_size / 2U;
	const auto tagged_base = base | top_byte;
	for (unsigned variant_index = 0; variant_index < variants.size();
	     ++variant_index) {
		const auto &variant = variants[variant_index];
		for (unsigned mode = 0; mode < 4; ++mode) {
			auto memory = std::vector<std::byte>(pair_test::page_size);
			pair_test::fill_memory(memory,
				0xc0U + variant_index * 11U + mode);
			auto expected_memory = memory;
			auto initial = pair_test::seeded_state(
				0x9500'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(variant_index) << 16) | mode);
			initial.tbi_enabled = true;
			initial.registers[base_register] = tagged_base;
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
					tagged_base + static_cast<std::uint64_t>(
						pair_test::signed_imm7(imm7) *
						static_cast<std::int64_t>(variant.size));
			char name[112];
			std::snprintf(name, sizeof(name),
				"%.*s top-byte-ignore mode=%u",
				static_cast<int>(variant.name.size()),
				variant.name.data(), mode);
			pair_test::check_memory(
				harness, name,
				pair_test::pair(variant.operation, true, variant.load,
					mode, imm7, base_register, first_register,
					second_register),
				initial, expected, native_requirement(), memory,
				variant.load ? std::span<const std::byte>{} :
					std::span<const std::byte>{expected_memory});
		}
	}
}

/*
 * The exact sequence Android 17's libhwui compiles for
 * SkPathGlue::cubicBezierCalculation. Its two LDP D pairs feed by-element
 * multiplies whose destination is also the element source, so neither a
 * dropped second destination nor an unzeroed first one is visible before the
 * final lane: t=1 then yields (0,0) instead of the (1,1) endpoint that
 * PathInterpolator.initPath() requires, and Android's zygote dies preloading
 * RippleAnimationSession. The base pointer carries a tag because Android
 * userspace enables top-byte-ignore.
 */
constexpr std::array<std::uint32_t, 8> cubic_control_points{
	0x0000'0000U, 0x0000'0000U, /* (0.0, 0.0) */
	0x3ecc'cccdU, 0x0000'0000U, /* (0.4, 0.0) */
	0x3e4c'cccdU, 0x3f80'0000U, /* (0.2, 1.0) */
	0x3f80'0000U, 0x3f80'0000U, /* (1.0, 1.0) */
};

constexpr std::array<std::uint32_t, 21> cubic_sequence{
	0x1e2e'1001U, /* fmov s1, #1.0            */
	0x1e20'0805U, /* fmul s5, s0, s0          */
	0x1e21'1002U, /* fmov s2, #3.0            */
	0x1e20'3821U, /* fsub s1, s1, s0          */
	0x1e21'0823U, /* fmul s3, s1, s1          */
	0x1e22'0864U, /* fmul s4, s3, s2          */
	0x1e22'0822U, /* fmul s2, s1, s2          */
	0x1e23'0821U, /* fmul s1, s1, s3          */
	0x1e24'0803U, /* fmul s3, s0, s4          */
	0x6d40'1804U, /* ldp  d4, d6, [x0]        */
	0x1e22'08a2U, /* fmul s2, s5, s2          */
	0x1e25'0800U, /* fmul s0, s0, s5          */
	0x0f81'9081U, /* fmul v1.2s, v4.2s, v1.s[0] */
	0x0f83'90c3U, /* fmul v3.2s, v6.2s, v3.s[0] */
	0x6d41'1804U, /* ldp  d4, d6, [x0, #16]   */
	0x0f82'9082U, /* fmul v2.2s, v4.2s, v2.s[0] */
	0x0e23'd421U, /* fadd v1.2s, v1.2s, v3.2s */
	0x0f80'90c0U, /* fmul v0.2s, v6.2s, v0.s[0] */
	0x0e22'd421U, /* fadd v1.2s, v1.2s, v2.2s */
	0x0e20'd420U, /* fadd v0.2s, v1.2s, v0.2s */
	0x5e0c'0401U, /* mov  s1, v0.s[1]         */
};

[[nodiscard]] float lane(
	const arm64::CpuState &cpu, unsigned reg, unsigned index)
{
	std::uint32_t bits = 0;
	std::memcpy(&bits, cpu.vectors[reg].bytes.data() + index * sizeof(bits),
		sizeof(bits));
	return std::bit_cast<float>(bits);
}

void write_lanes(
	arm64::CpuState &cpu, unsigned reg, float low, float high)
{
	const auto low_bits = std::bit_cast<std::uint32_t>(low);
	const auto high_bits = std::bit_cast<std::uint32_t>(high);
	cpu.vectors[reg].bytes.fill(std::byte{});
	std::memcpy(cpu.vectors[reg].bytes.data(), &low_bits, sizeof(low_bits));
	std::memcpy(cpu.vectors[reg].bytes.data() + sizeof(low_bits), &high_bits,
		sizeof(high_bits));
}

void write_scalar(arm64::CpuState &cpu, unsigned reg, float value)
{
	write_lanes(cpu, reg, value, 0.0F);
}

void multiply_by_element(
	arm64::CpuState &cpu, unsigned destination, unsigned source,
	unsigned element)
{
	const auto factor = lane(cpu, element, 0);
	const auto low = lane(cpu, source, 0) * factor;
	const auto high = lane(cpu, source, 1) * factor;
	write_lanes(cpu, destination, low, high);
}

void add_lanes(
	arm64::CpuState &cpu, unsigned destination, unsigned first,
	unsigned second)
{
	const auto low = lane(cpu, first, 0) + lane(cpu, second, 0);
	const auto high = lane(cpu, first, 1) + lane(cpu, second, 1);
	write_lanes(cpu, destination, low, high);
}

/* One statement per encoded instruction above, in the same order. Every
 * multiply and add at t=0 and t=1 is exact, so the host float model is the
 * architectural result without depending on the rounding mode. */
void apply_cubic_sequence(
	arm64::CpuState &cpu,
	std::span<const float, cubic_control_points.size()> points)
{
	write_scalar(cpu, 1, 1.0F);
	write_scalar(cpu, 5, lane(cpu, 0, 0) * lane(cpu, 0, 0));
	write_scalar(cpu, 2, 3.0F);
	write_scalar(cpu, 1, lane(cpu, 1, 0) - lane(cpu, 0, 0));
	write_scalar(cpu, 3, lane(cpu, 1, 0) * lane(cpu, 1, 0));
	write_scalar(cpu, 4, lane(cpu, 3, 0) * lane(cpu, 2, 0));
	write_scalar(cpu, 2, lane(cpu, 1, 0) * lane(cpu, 2, 0));
	write_scalar(cpu, 1, lane(cpu, 1, 0) * lane(cpu, 3, 0));
	write_scalar(cpu, 3, lane(cpu, 0, 0) * lane(cpu, 4, 0));
	write_lanes(cpu, 4, points[0], points[1]);
	write_lanes(cpu, 6, points[2], points[3]);
	write_scalar(cpu, 2, lane(cpu, 5, 0) * lane(cpu, 2, 0));
	write_scalar(cpu, 0, lane(cpu, 0, 0) * lane(cpu, 5, 0));
	multiply_by_element(cpu, 1, 4, 1);
	multiply_by_element(cpu, 3, 6, 3);
	write_lanes(cpu, 4, points[4], points[5]);
	write_lanes(cpu, 6, points[6], points[7]);
	multiply_by_element(cpu, 2, 4, 2);
	add_lanes(cpu, 1, 1, 3);
	multiply_by_element(cpu, 0, 6, 0);
	add_lanes(cpu, 1, 1, 2);
	add_lanes(cpu, 0, 1, 0);
	write_scalar(cpu, 1, lane(cpu, 0, 1));
}

void compiled_cubic_endpoints(engine_tests::EngineHarness &harness)
{
	constexpr std::uint64_t top_byte = 0x5600'0000'0000'0000ULL;
	constexpr std::size_t points_offset = 0x40;
	constexpr std::array parameters{0.0F, 1.0F};
	std::array<float, cubic_control_points.size()> points{};
	auto memory = std::vector<std::byte>(pair_test::page_size);
	pair_test::fill_memory(memory, 0x5cU);
	for (std::size_t index = 0; index < cubic_control_points.size(); ++index) {
		std::memcpy(memory.data() + points_offset +
				index * sizeof(std::uint32_t),
			&cubic_control_points[index], sizeof(std::uint32_t));
		points[index] = std::bit_cast<float>(cubic_control_points[index]);
	}
	std::array<engine_tests::ProgramWord, cubic_sequence.size() + 1U> code{};
	for (std::size_t index = 0; index < cubic_sequence.size(); ++index)
		code[index] = engine_tests::ProgramWord{
			.offset = index * sizeof(std::uint32_t),
			.instruction = cubic_sequence[index]};
	code.back() = engine_tests::ProgramWord{
		.offset = cubic_sequence.size() * sizeof(std::uint32_t),
		.instruction = 0xd400'0001U};
	const std::array regions{
		engine_tests::MemoryRegion{
			.address = pair_test::data_base,
			.initial = std::span<const std::byte>{memory},
		},
	};
	for (std::size_t index = 0; index < parameters.size(); ++index) {
		const auto parameter = parameters[index];
		auto initial = pair_test::seeded_state(
			0x9700'0000'0000'0000ULL | index);
		/* Round-to-nearest keeps the host float model exact, and the seeded
		 * FPSR already latches every cumulative exception bit. */
		initial.fpcr = 0;
		initial.tbi_enabled = true;
		initial.registers[0] =
			(pair_test::data_base + points_offset) | top_byte;
		write_scalar(initial, 0, parameter);
		auto expected = initial;
		apply_cubic_sequence(expected, points);
		expected.pc = engine_tests::engine_test_code_base +
			code.size() * sizeof(std::uint32_t);
		char name[112];
		std::snprintf(name, sizeof(name),
			"compiled cubic endpoint t=%g",
			static_cast<double>(parameter));
		harness.check_program({
			.name = name,
			.code = code,
			.initial = initial,
			.expected = expected,
			.native = native_requirement(),
			.memory = std::span<const engine_tests::MemoryRegion>{regions},
		});
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
	top_byte_ignore_transfers(harness);
	compiled_cubic_endpoints(harness);
	harness.print_summary("arm64-simd-load-store-pair");
}
