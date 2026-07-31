// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "uml_darwin_guest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace arm64_simd_structure_test {

namespace arm64 = uml::backend::arm64;
namespace engine = uml::backend::tests;

inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint64_t data_base = engine::engine_test_data_base;

enum class AddressMode : unsigned {
	no_offset,
	post_immediate,
	post_register,
};

struct MultipleForm final {
	unsigned opcode;
	unsigned register_count;
	unsigned structure_elements;
	std::string_view mnemonic;
};

/*
 * These are the seven allocated opcode partitions from the shared decode in
 * the architecture specification.  structure_elements==1 is the LD1/ST1
 * contiguous-register family; the remaining forms are de/interleaving
 * structures.
 */
inline constexpr std::array multiple_forms{
	MultipleForm{7, 1, 1, "LD1/ST1 one-register"},
	MultipleForm{10, 2, 1, "LD1/ST1 two-register"},
	MultipleForm{6, 3, 1, "LD1/ST1 three-register"},
	MultipleForm{2, 4, 1, "LD1/ST1 four-register"},
	MultipleForm{8, 2, 2, "LD2/ST2"},
	MultipleForm{4, 3, 3, "LD3/ST3"},
	MultipleForm{0, 4, 4, "LD4/ST4"},
};

struct SingleForm final {
	bool valid{};
	bool replicate{};
	unsigned register_count{};
	std::size_t element_size{};
	std::size_t lane{};
};

[[nodiscard]] inline engine::NativeRequirements native_requirement()
{
	return engine::NativeRequirements::features(
		uml::backend::ArmFeature::advanced_simd);
}

[[nodiscard]] inline constexpr bool multiple_arrangement_valid(
	const MultipleForm &form, bool full_vector, unsigned size)
{
	return form.structure_elements == 1U || full_vector || size != 3U;
}

[[nodiscard]] inline constexpr std::size_t vector_size(bool full_vector)
{
	return full_vector ? 16U : 8U;
}

[[nodiscard]] inline constexpr std::size_t multiple_transfer_size(
	const MultipleForm &form, bool full_vector)
{
	return form.register_count * vector_size(full_vector);
}

[[nodiscard]] inline constexpr std::uint32_t multiple_instruction(
	bool full_vector, bool load, bool post_index, unsigned offset_register,
	unsigned opcode, unsigned size, unsigned rn, unsigned rt)
{
	return 0x0c00'0000U | (static_cast<unsigned>(full_vector) << 30) |
		(static_cast<unsigned>(post_index) << 23) |
		(static_cast<unsigned>(load) << 22) |
		((offset_register & 31U) << 16) | ((opcode & 15U) << 12) |
		((size & 3U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint32_t single_instruction(
	bool full_vector, bool load, bool post_index, unsigned extra_register,
	unsigned offset_register, unsigned opcode, unsigned scale_bit,
	unsigned size, unsigned rn, unsigned rt)
{
	return 0x0d00'0000U | (static_cast<unsigned>(full_vector) << 30) |
		(static_cast<unsigned>(post_index) << 23) |
		(static_cast<unsigned>(load) << 22) |
		((extra_register & 1U) << 21) |
		((offset_register & 31U) << 16) | ((opcode & 7U) << 13) |
		((scale_bit & 1U) << 12) | ((size & 3U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

/*
 * Independent transcription of the shared single-structure decode:
 *   scale=00: byte lane, index Q:S:size
 *   scale=01: halfword lane, size<0> must be zero
 *   scale=10: word/doubleword lane, with the specified reserved cells
 *   scale=11: load-and-replicate only, S must be zero
 */
[[nodiscard]] inline constexpr SingleForm decode_single(
	bool full_vector, bool load, unsigned extra_register, unsigned opcode,
	unsigned scale_bit, unsigned size)
{
	const unsigned scale = opcode >> 1;
	const unsigned count = (opcode & 1U) != 0U ?
		3U + extra_register : 1U + extra_register;
	if (scale == 3U) {
		if (!load || scale_bit != 0U)
			return {};
		return {
			.valid = true,
			.replicate = true,
			.register_count = opcode == 6U ?
				1U + extra_register : 3U + extra_register,
			.element_size = std::size_t{1} << size,
		};
	}
	if (scale == 0U)
		return {
			.valid = true,
			.register_count = count,
			.element_size = 1,
			.lane = (static_cast<std::size_t>(full_vector) << 3) |
				(scale_bit << 2) | size,
		};
	if (scale == 1U) {
		if ((size & 1U) != 0U)
			return {};
		return {
			.valid = true,
			.register_count = count,
			.element_size = 2,
			.lane = (static_cast<std::size_t>(full_vector) << 2) |
				(scale_bit << 1) | (size >> 1),
		};
	}
	if ((size & 2U) != 0U ||
	    ((size & 1U) != 0U && scale_bit != 0U))
		return {};
	return {
		.valid = true,
		.register_count = count,
		.element_size = (size & 1U) != 0U ? 8U : 4U,
		.lane = (size & 1U) != 0U ?
			static_cast<std::size_t>(full_vector) :
			(static_cast<std::size_t>(full_vector) << 1) | scale_bit,
	};
}

[[nodiscard]] inline constexpr bool ordered_allocation_overlap(
	std::uint32_t instruction)
{
	return (instruction & 0xbfbf'fc00U) == 0x0d01'8400U;
}

[[nodiscard]] inline constexpr bool post_index(AddressMode mode)
{
	return mode != AddressMode::no_offset;
}

[[nodiscard]] inline constexpr unsigned encoded_offset_register(
	AddressMode mode, unsigned register_offset)
{
	switch (mode) {
	case AddressMode::no_offset:
		return 0;
	case AddressMode::post_immediate:
		return 31;
	case AddressMode::post_register:
		return register_offset;
	}
	return 0;
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			seed ^ (0xa500'0000'4000'0000ULL |
				(static_cast<std::uint64_t>(index) << 12) |
				(index * 0x101U));
	cpu.sp = (seed ^ 0x1234'5678'9abc'def0ULL) & ~std::uint64_t{15};
	cpu.nzcv = 0x9000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.fpmr = seed ^ 0x0000'0000'0012'0041ULL;
	cpu.tls = seed ^ 0xfedc'ba98'7654'3210ULL;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 43U + byte * 19U + 7U);
	return cpu;
}

inline void fill_memory(std::span<std::byte> memory, std::uint64_t seed)
{
	for (std::size_t offset = 0; offset < memory.size(); ++offset)
		memory[offset] = static_cast<std::byte>(
			0x80U | ((offset * 53U + seed * 29U + (offset >> 7)) & 0x7fU));
}

[[nodiscard]] inline std::uint64_t read_base(
	const arm64::CpuState &cpu, unsigned rn)
{
	return rn == 31U ? cpu.sp : cpu.registers[rn];
}

inline void write_base(arm64::CpuState &cpu, unsigned rn, std::uint64_t value)
{
	if (rn == 31U)
		cpu.sp = value;
	else
		cpu.registers[rn] = value;
}

inline void set_base(arm64::CpuState &cpu, unsigned rn, std::uint64_t value)
{
	write_base(cpu, rn, value);
}

inline void apply_writeback(
	const arm64::CpuState &initial, arm64::CpuState &expected,
	AddressMode mode, unsigned rn, unsigned rm, std::size_t immediate)
{
	if (mode == AddressMode::no_offset)
		return;
	const auto increment = mode == AddressMode::post_immediate ?
		static_cast<std::uint64_t>(immediate) : initial.registers[rm];
	write_base(expected, rn, read_base(initial, rn) + increment);
}

inline void apply_multiple_oracle(
	const arm64::CpuState &initial, arm64::CpuState &expected,
	std::span<const std::byte> initial_memory,
	std::span<std::byte> expected_memory, std::size_t memory_offset,
	const MultipleForm &form, bool full_vector, unsigned size, bool load,
	unsigned first_register)
{
	const auto bytes_per_vector = vector_size(full_vector);
	const auto bytes_per_element = std::size_t{1} << size;
	for (unsigned reg = 0; reg < form.register_count; ++reg) {
		auto &destination =
			expected.vectors[(first_register + reg) & 31U].bytes;
		const auto &source =
			initial.vectors[(first_register + reg) & 31U].bytes;
		if (load)
			destination.fill(std::byte{});
		for (std::size_t byte = 0; byte < bytes_per_vector; ++byte) {
			std::size_t transfer_offset;
			if (form.structure_elements == 1U) {
				transfer_offset = reg * bytes_per_vector + byte;
			} else {
				const auto lane = byte / bytes_per_element;
				const auto lane_byte = byte % bytes_per_element;
				transfer_offset =
					(lane * form.register_count + reg) *
						bytes_per_element +
					lane_byte;
			}
			if (load)
				destination[byte] =
					initial_memory[memory_offset + transfer_offset];
			else
				expected_memory[memory_offset + transfer_offset] =
					source[byte];
		}
	}
}

inline void apply_single_oracle(
	const arm64::CpuState &initial, arm64::CpuState &expected,
	std::span<const std::byte> initial_memory,
	std::span<std::byte> expected_memory, std::size_t memory_offset,
	const SingleForm &form, bool full_vector, bool load,
	unsigned first_register)
{
	const auto bytes_per_vector = vector_size(full_vector);
	for (unsigned reg = 0; reg < form.register_count; ++reg) {
		auto &destination =
			expected.vectors[(first_register + reg) & 31U].bytes;
		const auto &source =
			initial.vectors[(first_register + reg) & 31U].bytes;
		const auto transfer_offset =
			memory_offset + reg * form.element_size;
		if (form.replicate) {
			destination.fill(std::byte{});
			for (std::size_t byte = 0; byte < bytes_per_vector;
			     byte += form.element_size)
				std::copy_n(
					initial_memory.begin() +
						static_cast<std::ptrdiff_t>(transfer_offset),
					form.element_size, destination.begin() +
						static_cast<std::ptrdiff_t>(byte));
		} else if (load) {
			std::copy_n(
				initial_memory.begin() +
					static_cast<std::ptrdiff_t>(transfer_offset),
				form.element_size,
				destination.begin() +
					static_cast<std::ptrdiff_t>(
						form.lane * form.element_size));
		} else {
			std::copy_n(
				source.begin() + static_cast<std::ptrdiff_t>(
					form.lane * form.element_size),
				form.element_size,
				expected_memory.begin() +
					static_cast<std::ptrdiff_t>(transfer_offset));
		}
	}
}

inline void check_memory(
	engine::EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial,
	const arm64::CpuState &expected,
	std::span<const std::byte> initial_memory,
	std::span<const std::byte> expected_memory = {},
	engine::ExpectedEvent event = engine::ExpectedEvent::system_call,
	engine::MemoryFaultExpectation fault = {}, bool compare_fault = false,
	std::span<const engine::AllocationTag> initial_tags = {},
	std::span<const engine::AllocationTag> expected_tags = {})
{
	const std::array regions{
		engine::MemoryRegion{
			.address = data_base,
			.initial = initial_memory,
			.expected = expected_memory,
			.initial_tags = initial_tags,
			.expected_tags = expected_tags,
		},
	};
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = expected,
		.event = event,
		.native = native_requirement(),
		.fault = fault,
		.compare_fault = compare_fault,
		.memory = std::span<const engine::MemoryRegion>{regions},
	});
}

inline void check_multiple_success(
	engine::EngineHarness &harness, std::string_view name,
	const MultipleForm &form, bool full_vector, unsigned size, bool load,
	AddressMode mode, unsigned rn, unsigned rt, unsigned rm,
	std::uint64_t address, std::span<const std::byte> initial_memory,
	std::uint64_t seed)
{
	auto initial = seeded_state(seed);
	set_base(initial, rn, address);
	auto expected = initial;
	auto expected_memory = std::vector<std::byte>(
		initial_memory.begin(), initial_memory.end());
	const auto memory_offset =
		static_cast<std::size_t>(address - data_base);
	apply_multiple_oracle(initial, expected, initial_memory, expected_memory,
		memory_offset, form, full_vector, size, load, rt);
	apply_writeback(initial, expected, mode, rn, rm,
		multiple_transfer_size(form, full_vector));
	check_memory(harness, name,
		multiple_instruction(full_vector, load, post_index(mode),
			encoded_offset_register(mode, rm), form.opcode, size, rn, rt),
		initial, expected, initial_memory, expected_memory);
}

inline void check_single_success(
	engine::EngineHarness &harness, std::string_view name,
	const SingleForm &form, bool full_vector, bool load,
	unsigned extra_register, unsigned opcode, unsigned scale_bit,
	unsigned size, AddressMode mode, unsigned rn, unsigned rt, unsigned rm,
	std::uint64_t address, std::span<const std::byte> initial_memory,
	std::uint64_t seed)
{
	auto initial = seeded_state(seed);
	set_base(initial, rn, address);
	auto expected = initial;
	auto expected_memory = std::vector<std::byte>(
		initial_memory.begin(), initial_memory.end());
	const auto memory_offset =
		static_cast<std::size_t>(address - data_base);
	apply_single_oracle(initial, expected, initial_memory, expected_memory,
		memory_offset, form, full_vector, load, rt);
	const auto transfer_size = form.register_count * form.element_size;
	apply_writeback(initial, expected, mode, rn, rm, transfer_size);
	check_memory(harness, name,
		single_instruction(full_vector, load, post_index(mode),
			extra_register, encoded_offset_register(mode, rm), opcode,
			scale_bit, size, rn, rt),
		initial, expected, initial_memory, expected_memory);
}

} // namespace arm64_simd_structure_test
