// SPDX-License-Identifier: GPL-2.0-or-later
#include "program.hpp"

#include "cpu/arm64/layout/assembly_layout.hpp"

#include "../detail/atomic_floating.hpp"
#include "../detail/floating_compare.hpp"
#include "../detail/bfloat_conversion.hpp"
#include "../detail/bfloat_dot.hpp"
#include "../detail/bfloat_matrix.hpp"
#include "../detail/bfloat_multiply.hpp"
#include "../detail/half_fused_long.hpp"
#include "../detail/half_dot.hpp"
#include "../detail/half_matrix.hpp"
#include "../detail/floating_complex.hpp"
#include "../detail/floating_arithmetic.hpp"
#include "../detail/floating_conversion.hpp"
#include "../detail/floating_fused.hpp"
#include "../detail/floating_horizontal.hpp"
#include "../detail/floating_indexed.hpp"
#include "../detail/floating_integer_conversion.hpp"
#include "../detail/floating_reciprocal.hpp"
#include "../detail/floating_reciprocal_software.hpp"
#include "../detail/floating_round.hpp"
#include "../detail/floating_scale.hpp"
#include "../detail/fp_state_access.hpp"
#include "../detail/fp8.hpp"
#include "../detail/gcs.hpp"
#include "../detail/instruction_helpers.hpp"
#include "../detail/integer_multiply.hpp"
#include "../detail/pointer_auth.hpp"
#include "cpu/host_capabilities.hpp"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace {

template<typename Floating>
using HorizontalInteger = std::conditional_t<sizeof(Floating) == 2,
	std::uint16_t, std::conditional_t<sizeof(Floating) == 4,
		std::uint32_t, std::uint64_t>>;

template<typename Floating>
[[nodiscard]] Floating decode_horizontal_lane(
	const std::array<std::byte, 16> &source, unsigned lane) noexcept
{
	return std::bit_cast<Floating>(static_cast<HorizontalInteger<Floating>>(
		uml::cpu::arm64::detail::decode_little_endian(
			std::span{source}.subspan(lane * sizeof(Floating),
				sizeof(Floating)))));
}

template<typename Floating>
void encode_horizontal_lane(std::array<std::byte, 16> &destination,
	unsigned lane, Floating value) noexcept
{
	uml::cpu::arm64::detail::encode_little_endian(
		std::bit_cast<HorizontalInteger<Floating>>(value),
		std::span{destination}.subspan(lane * sizeof(Floating),
			sizeof(Floating)));
}

template<typename Floating>
[[nodiscard]] Floating native_horizontal_arithmetic(
	uml::cpu::arm64::detail::FloatingHorizontalOperation operation,
	Floating first, Floating second) noexcept
{
	Floating result;
#define UML_HORIZONTAL_ARITHMETIC_ASM(suffix, instruction) \
	__asm__ volatile(#instruction " %" #suffix "0, %" #suffix "1, %" #suffix \
		"2" : "=w"(result) : "w"(first), "w"(second))
#define UML_HORIZONTAL_ARITHMETIC_WIDTH(suffix) \
	do { \
		if (operation == uml::cpu::arm64::detail:: \
			FloatingHorizontalOperation::absolute_difference) \
			UML_HORIZONTAL_ARITHMETIC_ASM(suffix, fabd); \
		else \
			UML_HORIZONTAL_ARITHMETIC_ASM(suffix, fadd); \
	} while (false)
	if constexpr (sizeof(Floating) == 2U)
		UML_HORIZONTAL_ARITHMETIC_WIDTH(h);
	else if constexpr (sizeof(Floating) == 4U)
		UML_HORIZONTAL_ARITHMETIC_WIDTH(s);
	else
		UML_HORIZONTAL_ARITHMETIC_WIDTH(d);
#undef UML_HORIZONTAL_ARITHMETIC_WIDTH
#undef UML_HORIZONTAL_ARITHMETIC_ASM
	return result;
}

template<typename Floating>
[[nodiscard]] Floating horizontal_ah_element(
	uml::cpu::arm64::CpuState &cpu,
	uml::cpu::arm64::detail::FloatingHorizontalOperation operation,
	Floating first, Floating second) noexcept
{
	using Integer = HorizontalInteger<Floating>;
	const auto first_bits = std::bit_cast<Integer>(first);
	const auto second_bits = std::bit_cast<Integer>(second);
	if (operation == uml::cpu::arm64::detail::
			FloatingHorizontalOperation::absolute_difference ||
	    operation == uml::cpu::arm64::detail::
			FloatingHorizontalOperation::add) {
		const auto prepared =
			uml::cpu::arm64::detail::
				prepare_floating_controlled_arithmetic(
					first_bits, second_bits, cpu.fpcr,
					operation == uml::cpu::arm64::detail::
						FloatingHorizontalOperation::
							absolute_difference);
		cpu.fpsr |= prepared.exceptions;
		if (prepared.complete)
			return std::bit_cast<Floating>(prepared.bits);
		first = std::bit_cast<Floating>(prepared.first);
		second = std::bit_cast<Floating>(prepared.second);
		std::uint64_t saved_fpcr{};
		std::uint64_t saved_fpsr{};
		__asm__ volatile("mrs %0, fpcr" : "=r"(saved_fpcr) : : "memory");
		__asm__ volatile("mrs %0, fpsr" : "=r"(saved_fpsr) : : "memory");
		const auto local_fpcr = (cpu.fpcr & 2U) != 0U ?
			saved_fpcr & ~((std::uint64_t{1} << 24) |
				(std::uint64_t{1} << 19)) : saved_fpcr;
		if (local_fpcr != saved_fpcr)
			__asm__ volatile("msr fpcr, %0" : :
				"r"(local_fpcr) : "memory");
		__asm__ volatile("msr fpsr, xzr" : : : "memory");
		const auto result =
			native_horizontal_arithmetic(operation, first, second);
		std::uint64_t raised{};
		__asm__ volatile("mrs %0, fpsr" : "=r"(raised) : : "memory");
		__asm__ volatile("msr fpsr, %0" : :
			"r"(saved_fpsr) : "memory");
		if (local_fpcr != saved_fpcr)
			__asm__ volatile("msr fpcr, %0" : :
				"r"(saved_fpcr) : "memory");
		const auto completed =
			uml::cpu::arm64::detail::
				complete_floating_controlled_arithmetic(
					std::bit_cast<Integer>(result), cpu.fpcr,
					static_cast<std::uint32_t>(raised));
		cpu.fpsr |= completed.exceptions;
		return std::bit_cast<Floating>(completed.bits);
	}
	if (operation == uml::cpu::arm64::detail::
			FloatingHorizontalOperation::absolute_maximum ||
	    operation == uml::cpu::arm64::detail::
			FloatingHorizontalOperation::absolute_minimum) {
		const auto result =
			uml::cpu::arm64::detail::floating_absolute_minmax(
				first_bits, second_bits,
				operation == uml::cpu::arm64::detail::
					FloatingHorizontalOperation::absolute_maximum,
				(cpu.fpcr & (1U << 25)) != 0U);
		cpu.fpsr |= result.exceptions;
		return std::bit_cast<Floating>(result.bits);
	}
	const auto result =
		uml::cpu::arm64::detail::floating_alternate_minmax(
			first_bits, second_bits,
			operation == uml::cpu::arm64::detail::
					FloatingHorizontalOperation::maximum ||
				operation == uml::cpu::arm64::detail::
					FloatingHorizontalOperation::maximum_number,
			cpu.fpcr,
			operation == uml::cpu::arm64::detail::
					FloatingHorizontalOperation::maximum_number ||
				operation == uml::cpu::arm64::detail::
					FloatingHorizontalOperation::minimum_number);
	cpu.fpsr |= result.exceptions;
	return std::bit_cast<Floating>(result.bits);
}

template<typename Floating>
void floating_horizontal_ah_software(uml::cpu::arm64::CpuState &cpu,
	unsigned destination, unsigned first_register, unsigned second_register,
	uml::cpu::arm64::detail::FloatingHorizontalOperation operation,
	uml::cpu::arm64::detail::FloatingHorizontalForm form,
	unsigned vector_bytes) noexcept
{
	const auto first_source = cpu.vectors[first_register].bytes;
	const auto second_source = cpu.vectors[second_register].bytes;
	std::array<std::byte, 16> result{};
	if (operation == uml::cpu::arm64::detail::
			FloatingHorizontalOperation::absolute_maximum ||
	    operation == uml::cpu::arm64::detail::
			FloatingHorizontalOperation::absolute_minimum) {
		using Integer = HorizontalInteger<Floating>;
		const unsigned lanes = vector_bytes / sizeof(Integer);
		for (unsigned lane = 0; lane < lanes; ++lane) {
			const auto first = static_cast<Integer>(
				uml::cpu::arm64::detail::decode_little_endian(
					std::span{first_source}.subspan(
						lane * sizeof(Integer), sizeof(Integer))));
			const auto second = static_cast<Integer>(
				uml::cpu::arm64::detail::decode_little_endian(
					std::span{second_source}.subspan(
						lane * sizeof(Integer), sizeof(Integer))));
			const auto value =
				uml::cpu::arm64::detail::floating_absolute_minmax(
					first, second,
					operation == uml::cpu::arm64::detail::
						FloatingHorizontalOperation::absolute_maximum,
					(cpu.fpcr & (1U << 25)) != 0U);
			uml::cpu::arm64::detail::encode_little_endian(value.bits,
				std::span{result}.subspan(
					lane * sizeof(Integer), sizeof(Integer)));
			cpu.fpsr |= value.exceptions;
		}
		cpu.vectors[destination].bytes = result;
		return;
	}
	if (form == uml::cpu::arm64::detail::
			FloatingHorizontalForm::scalar_binary &&
	    (cpu.fpcr & 4U) != 0U)
		result = first_source;
	const auto apply = [&](Floating first, Floating second) {
		return horizontal_ah_element(cpu, operation, first, second);
	};
	using Form = uml::cpu::arm64::detail::FloatingHorizontalForm;
	if (form == Form::scalar_binary) {
		encode_horizontal_lane(result, 0, apply(
			decode_horizontal_lane<Floating>(first_source, 0),
			decode_horizontal_lane<Floating>(second_source, 0)));
	} else if (form == Form::scalar_pairwise) {
		encode_horizontal_lane(result, 0, apply(
			decode_horizontal_lane<Floating>(first_source, 0),
			decode_horizontal_lane<Floating>(first_source, 1)));
	} else if (form == Form::vector_binary) {
		const unsigned lanes = vector_bytes / sizeof(Floating);
		for (unsigned lane = 0; lane < lanes; ++lane)
			encode_horizontal_lane(result, lane, apply(
				decode_horizontal_lane<Floating>(first_source, lane),
				decode_horizontal_lane<Floating>(second_source, lane)));
	} else if (form == Form::vector_pairwise) {
		const unsigned lanes = vector_bytes / sizeof(Floating);
		for (unsigned lane = 0; lane < lanes; ++lane) {
			const auto &source = lane < lanes / 2U ?
				first_source : second_source;
			const unsigned pair = (lane % (lanes / 2U)) * 2U;
			encode_horizontal_lane(result, lane, apply(
				decode_horizontal_lane<Floating>(source, pair),
				decode_horizontal_lane<Floating>(source, pair + 1U)));
		}
	} else {
		const unsigned lanes = vector_bytes / sizeof(Floating);
		std::array<Floating, 8> values{};
		for (unsigned lane = 0; lane < lanes; ++lane)
			values[lane] = decode_horizontal_lane<Floating>(first_source, lane);
		for (unsigned remaining = lanes; remaining > 1U; remaining /= 2U)
			for (unsigned lane = 0; lane < remaining / 2U; ++lane)
				values[lane] = apply(values[lane * 2U],
					values[lane * 2U + 1U]);
		encode_horizontal_lane(result, 0, values[0]);
	}
	cpu.vectors[destination].bytes = result;
}

template<typename Integer, unsigned VectorBytes>
void floating_scale_software(uml::cpu::arm64::CpuState &cpu,
	std::uintptr_t operand) noexcept
{
	const unsigned destination = operand & 31U;
	const unsigned first_register = (operand >> 8) & 31U;
	const unsigned second_register = (operand >> 16) & 31U;
	const auto first = cpu.vectors[first_register].bytes;
	const auto second = cpu.vectors[second_register].bytes;
	std::array<std::byte, 16> result{};
	using Signed = std::make_signed_t<Integer>;
	for (unsigned lane = 0; lane < VectorBytes / sizeof(Integer); ++lane) {
		const auto value = static_cast<Integer>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{first}.subspan(
					lane * sizeof(Integer), sizeof(Integer))));
		const auto scale_bits = static_cast<Integer>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{second}.subspan(
					lane * sizeof(Integer), sizeof(Integer))));
		const auto scaled = uml::cpu::arm64::detail::scale_floating(
			value, sizeof(Integer) * 8U, static_cast<std::int64_t>(
				std::bit_cast<Signed>(scale_bits)), cpu.fpcr);
		uml::cpu::arm64::detail::encode_little_endian(scaled.bits,
			std::span{result}.subspan(
				lane * sizeof(Integer), sizeof(Integer)));
		cpu.fpsr |= scaled.exceptions;
	}
	cpu.vectors[destination].bytes = result;
}

} // namespace

extern "C" [[gnu::noinline]] void uml_arm64_floating_horizontal_ah_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	using Operation =
		uml::cpu::arm64::detail::FloatingHorizontalOperation;
	using Form = uml::cpu::arm64::detail::FloatingHorizontalForm;
	const unsigned destination = operand & 31U;
	const unsigned first_register = (operand >> 8) & 31U;
	const unsigned second_register = (operand >> 16) & 31U;
	const auto operation = static_cast<Operation>((operand >> 24) & 15U);
	const auto form = static_cast<Form>((operand >> 28) & 15U);
	const unsigned width_code = (operand >> 32) & 3U;
	const unsigned vector_bytes = (operand & (std::uintptr_t{1} << 34)) != 0U ?
		16U : 8U;
	if (width_code == 0U)
		floating_horizontal_ah_software<_Float16>(*cpu, destination,
			first_register, second_register, operation, form, vector_bytes);
	else if (width_code == 1U)
		floating_horizontal_ah_software<float>(*cpu, destination,
			first_register, second_register, operation, form, vector_bytes);
	else
		floating_horizontal_ah_software<double>(*cpu, destination,
			first_register, second_register, operation, form, vector_bytes);
}

namespace {

template<typename Floating>
void simd_indexed_floating_software(
	uml::cpu::arm64::CpuState &cpu,
	std::uint32_t instruction) noexcept
{
	using Integer = std::conditional_t<sizeof(Floating) == 2U,
		std::uint16_t, std::conditional_t<sizeof(Floating) == 4U,
			std::uint32_t, std::uint64_t>>;
	using Operation =
		uml::cpu::arm64::detail::IndexedFloatingOperation;
	const unsigned destination = instruction & 31U;
	const unsigned left_register = (instruction >> 5) & 31U;
	unsigned element_register = (instruction >> 16) & 15U;
	const bool scalar = (instruction & 0x1000'0000U) != 0U;
	const bool q = (instruction & 0x4000'0000U) != 0U;
	const bool extended = (instruction & 0x2000'0000U) != 0U;
	const unsigned operation_bits = (instruction >> 12) & 15U;
	unsigned index;
	if constexpr (sizeof(Floating) == 2U) {
		index = ((instruction >> 9) & 4U) |
			((instruction >> 20) & 1U) |
			((instruction >> 20) & 2U);
	} else {
		element_register |= ((instruction >> 20) & 1U) << 4;
		if constexpr (sizeof(Floating) == 4U)
			index = ((instruction >> 10) & 2U) |
				((instruction >> 21) & 1U);
		else
			index = (instruction >> 11) & 1U;
	}
	const auto left_source = cpu.vectors[left_register].bytes;
	const auto element_source = cpu.vectors[element_register].bytes;
	const auto accumulator = cpu.vectors[destination].bytes;
	std::array<std::byte, 16> result{};
	const bool accumulating = operation_bits == 1U ||
		operation_bits == 5U;
	if (scalar && (cpu.fpcr & 4U) != 0U)
		result = accumulating ? accumulator : left_source;
	const auto selected_bits = static_cast<Integer>(
		uml::cpu::arm64::detail::decode_little_endian(
			std::span{element_source}.subspan(
				index * sizeof(Floating), sizeof(Floating))));
	const auto selected = std::bit_cast<Floating>(selected_bits);
	const unsigned active_bytes = scalar ? sizeof(Floating) : q ? 16U : 8U;
	const auto operation = operation_bits == 1U ? Operation::add :
		operation_bits == 5U ? Operation::subtract :
		extended ? Operation::multiply_extended : Operation::multiply;
	for (unsigned lane = 0; lane < active_bytes / sizeof(Floating); ++lane) {
		const auto left_bits = static_cast<Integer>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{left_source}.subspan(
					lane * sizeof(Floating), sizeof(Floating))));
		const auto addend_bits = static_cast<Integer>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{accumulator}.subspan(
					lane * sizeof(Floating), sizeof(Floating))));
		const auto lane_result =
			uml::cpu::arm64::detail::indexed_floating_software(
				std::bit_cast<Floating>(addend_bits),
				std::bit_cast<Floating>(left_bits), selected,
				operation, cpu.fpcr);
		uml::cpu::arm64::detail::encode_little_endian(
			lane_result.bits, std::span{result}.subspan(
				lane * sizeof(Floating), sizeof(Floating)));
		cpu.fpsr |= lane_result.exceptions;
	}
	cpu.vectors[destination].bytes = result;
}

} // namespace

extern "C" [[gnu::noinline]] void
uml_arm64_simd_indexed_floating_software(
	uml::cpu::arm64::CpuState *cpu,
	std::uint32_t instruction) noexcept
{
	const unsigned size = (instruction >> 22) & 3U;
	if (size == 0U)
		simd_indexed_floating_software<_Float16>(*cpu, instruction);
	else if (size == 2U)
		simd_indexed_floating_software<float>(*cpu, instruction);
	else
		simd_indexed_floating_software<double>(*cpu, instruction);
}

extern "C" [[gnu::noinline]] void uml_arm64_floating_scale_software_4h(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	floating_scale_software<std::uint16_t, 8U>(*cpu, operand);
}

extern "C" [[gnu::noinline]] void uml_arm64_floating_scale_software_8h(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	floating_scale_software<std::uint16_t, 16U>(*cpu, operand);
}

extern "C" [[gnu::noinline]] void uml_arm64_floating_scale_software_2s(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	floating_scale_software<std::uint32_t, 8U>(*cpu, operand);
}

extern "C" [[gnu::noinline]] void uml_arm64_floating_scale_software_4s(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	floating_scale_software<std::uint32_t, 16U>(*cpu, operand);
}

extern "C" [[gnu::noinline]] void uml_arm64_floating_scale_software_2d(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	floating_scale_software<std::uint64_t, 16U>(*cpu, operand);
}

extern "C" [[gnu::noinline]] void uml_arm64_bfloat_conversion_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto destination_offset = operand & 0xffffU;
	const auto source_offset = (operand >> 16) & 0xffffU;
	const bool scalar = (operand & (std::uintptr_t{1} << 32)) != 0U;
	const bool upper = (operand & (std::uintptr_t{1} << 33)) != 0U;
	auto *const base = reinterpret_cast<std::byte *>(cpu);
	auto &destination = *reinterpret_cast<
		uml::cpu::arm64::VectorRegister *>(base + destination_offset);
	const auto source = reinterpret_cast<
		const uml::cpu::arm64::VectorRegister *>(
			base + source_offset)->bytes;
	auto result = scalar && (cpu->fpcr & 4U) != 0U ?
		destination.bytes :
		upper ? destination.bytes :
			std::array<std::byte, 16>{};
	const unsigned lanes = scalar ? 1U : 4U;
	const unsigned first_lane = upper ? 4U : 0U;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto input = static_cast<std::uint32_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{source}.subspan(lane * 4U, 4U)));
		const auto converted =
			uml::cpu::arm64::detail::convert_to_bfloat(input, cpu->fpcr);
		uml::cpu::arm64::detail::encode_little_endian(converted.bits,
			std::span{result}.subspan((first_lane + lane) * 2U, 2U));
		cpu->fpsr |= converted.exceptions;
	}
	destination.bytes = result;
}

extern "C" [[gnu::noinline]] void uml_arm64_bfloat_multiply_long_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto vector = [cpu, operand](unsigned shift)
		-> uml::cpu::arm64::VectorRegister & {
		const auto offset = (operand >> shift) & 0xffffU;
		return *reinterpret_cast<uml::cpu::arm64::VectorRegister *>(
			reinterpret_cast<std::byte *>(cpu) + offset);
	};
	auto &destination = vector(0U);
	const auto addends = destination.bytes;
	const auto first = vector(16U).bytes;
	const auto second = vector(32U).bytes;
	const bool top = (operand & (std::uintptr_t{1} << 48)) != 0U;
	const bool by_element = (operand & (std::uintptr_t{1} << 49)) != 0U;
	const unsigned index = (operand >> 50) & 7U;
	auto result = addends;
	for (unsigned lane = 0; lane < 4U; ++lane) {
		const unsigned first_lane = lane * 2U + top;
		const unsigned second_lane = by_element ? index : lane * 2U + top;
		const auto addend = static_cast<std::uint32_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{addends}.subspan(lane * 4U, 4U)));
		const auto left = static_cast<std::uint16_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{first}.subspan(first_lane * 2U, 2U)));
		const auto right = static_cast<std::uint16_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{second}.subspan(second_lane * 2U, 2U)));
		uml::cpu::arm64::detail::encode_little_endian(
			uml::cpu::arm64::detail::bfloat_multiply_add(
				addend, left, right, cpu->fpcr),
			std::span{result}.subspan(lane * 4U, 4U));
	}
	destination.bytes = result;
}

extern "C" [[gnu::noinline]] void uml_arm64_bfloat_dot_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto vector = [cpu, operand](unsigned shift)
		-> uml::cpu::arm64::VectorRegister & {
		const auto offset = (operand >> shift) & 0xffffU;
		return *reinterpret_cast<uml::cpu::arm64::VectorRegister *>(
			reinterpret_cast<std::byte *>(cpu) + offset);
	};
	auto &destination = vector(0U);
	const auto addends = destination.bytes;
	const auto first = vector(16U).bytes;
	const auto second = vector(32U).bytes;
	const bool by_element = (operand & (std::uintptr_t{1} << 48)) != 0U;
	const bool wide = (operand & (std::uintptr_t{1} << 49)) != 0U;
	const unsigned index = (operand >> 50) & 3U;
	std::array<std::byte, 16> result{};
	const unsigned lanes = wide ? 4U : 2U;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const unsigned first_pair = lane * 2U;
		const unsigned second_pair = by_element ? index * 2U : first_pair;
		const auto read_bfloat = [](const auto &source, unsigned element) {
			return static_cast<std::uint16_t>(
				uml::cpu::arm64::detail::decode_little_endian(
					std::span{source}.subspan(element * 2U, 2U)));
		};
		const auto addend = static_cast<std::uint32_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{addends}.subspan(lane * 4U, 4U)));
		uml::cpu::arm64::detail::encode_little_endian(
			uml::cpu::arm64::detail::bfloat_dot_add(addend,
				read_bfloat(first, first_pair),
				read_bfloat(first, first_pair + 1U),
				read_bfloat(second, second_pair),
				read_bfloat(second, second_pair + 1U), cpu->fpcr),
			std::span{result}.subspan(lane * 4U, 4U));
	}
	destination.bytes = result;
}

extern "C" [[gnu::noinline]] void uml_arm64_bfloat_matrix_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto vector = [cpu, operand](unsigned shift)
		-> uml::cpu::arm64::VectorRegister & {
		const auto offset = (operand >> shift) & 0xffffU;
		return *reinterpret_cast<uml::cpu::arm64::VectorRegister *>(
			reinterpret_cast<std::byte *>(cpu) + offset);
	};
	auto &destination = vector(0U);
	const auto addends = destination.bytes;
	const auto left = vector(16U).bytes;
	const auto right = vector(32U).bytes;
	destination.bytes =
		uml::cpu::arm64::detail::bfloat_matrix_multiply_add(
			addends, left, right, cpu->fpcr);
}

extern "C" [[gnu::noinline]] void uml_arm64_half_fused_long_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto vector = [cpu, operand](unsigned shift)
		-> uml::cpu::arm64::VectorRegister & {
		const auto offset = (operand >> shift) & 0xffffU;
		return *reinterpret_cast<uml::cpu::arm64::VectorRegister *>(
			reinterpret_cast<std::byte *>(cpu) + offset);
	};
	const unsigned descriptor = operand >> 48U;
	const bool by_element = (descriptor & 1U) != 0U;
	const bool subtract = (descriptor & 2U) != 0U;
	const bool upper = (descriptor & 4U) != 0U;
	const bool wide = (descriptor & 8U) != 0U;
	const unsigned index = (descriptor >> 4U) & 7U;
	auto &destination = vector(0U);
	const auto addends = destination.bytes;
	const auto first = vector(16U).bytes;
	const auto second = vector(32U).bytes;
	std::array<std::byte, 16> result{};
	const unsigned lanes = wide ? 4U : 2U;
	const unsigned first_lane = upper ? lanes : 0U;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const unsigned second_lane = by_element ? index : first_lane + lane;
		const auto addend = static_cast<std::uint32_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{addends}.subspan(lane * 4U, 4U)));
		const auto left = static_cast<std::uint16_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{first}.subspan((first_lane + lane) * 2U, 2U)));
		const auto right = static_cast<std::uint16_t>(
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{second}.subspan(second_lane * 2U, 2U)));
		const auto fused = uml::cpu::arm64::detail::half_fused_multiply_add(
			addend, left, right, subtract, cpu->fpcr);
		uml::cpu::arm64::detail::encode_little_endian(fused.bits,
			std::span{result}.subspan(lane * 4U, 4U));
		cpu->fpsr |= fused.exceptions;
	}
	destination.bytes = result;
}

extern "C" [[gnu::noinline]] void uml_arm64_half_matrix_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto vector = [cpu, operand](unsigned shift)
		-> uml::cpu::arm64::VectorRegister & {
		const auto offset = (operand >> shift) & 0xffffU;
		return *reinterpret_cast<uml::cpu::arm64::VectorRegister *>(
			reinterpret_cast<std::byte *>(cpu) + offset);
	};
	auto &destination = vector(0U);
	const auto addends = destination.bytes;
	const auto first = vector(16U).bytes;
	const auto second = vector(32U).bytes;
	const bool widening = (operand >> 48U) != 0U;
	const auto result = uml::cpu::arm64::detail::half_matrix_multiply_add(
		addends, first, second, widening, cpu->fpcr);
	destination.bytes = result.bytes;
	cpu->fpsr |= result.exceptions;
}

extern "C" [[gnu::noinline]] void uml_arm64_half_dot_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto vector = [cpu](unsigned offset)
		-> uml::cpu::arm64::VectorRegister & {
		return *reinterpret_cast<uml::cpu::arm64::VectorRegister *>(
			reinterpret_cast<std::byte *>(cpu) + offset);
	};
	const unsigned destination = operand & 0xffffU;
	const unsigned first_offset = (operand >> 16U) & 0xffffU;
	const unsigned second_offset = (operand >> 32U) & 0xffffU;
	const unsigned descriptor = operand >> 48U;
	const bool by_element = (descriptor & 1U) != 0U;
	const bool wide = (descriptor & 2U) != 0U;
	const unsigned index = (descriptor >> 2U) & 3U;
	const auto addends = vector(destination).bytes;
	const auto first = vector(first_offset).bytes;
	const auto second = vector(second_offset).bytes;
	const auto result = uml::cpu::arm64::detail::half_dot_add(addends,
		first, second, {true, by_element, wide, index}, cpu->fpcr);
	vector(destination).bytes = result.bytes;
	cpu->fpsr |= result.exceptions;
}

extern "C" [[gnu::noinline]]
uml::cpu::arm64::detail::FloatingPrecisionConversion
uml_arm64_convert_floating_precision(std::uint64_t input,
	unsigned source_width, unsigned destination_width, std::uint32_t fpcr) noexcept
{
	return uml::cpu::arm64::detail::convert_floating_precision(
		input, source_width, destination_width, fpcr);
}

extern "C" [[gnu::noinline]] void
uml_arm64_simd_floating_precision_vector_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const unsigned destination = operand & 31U;
	const unsigned source_register = (operand >> 8) & 31U;
	const unsigned shape = (operand >> 16) & 7U;
	const bool upper = ((operand >> 20) & 1U) != 0U;
	const bool scalar = ((operand >> 21) & 1U) != 0U;
	constexpr std::array source_widths{16U, 32U, 32U, 64U, 64U};
	constexpr std::array destination_widths{32U, 64U, 16U, 32U, 32U};
	const unsigned source_width = source_widths[shape];
	const unsigned destination_width = destination_widths[shape];
	const bool narrow = shape >= 2U;
	const bool round_odd = shape == 4U;
	const unsigned lanes = scalar ? 1U :
		(narrow ? 128U : 64U) / source_width;
	const unsigned source_bytes = source_width / 8U;
	const unsigned destination_bytes = destination_width / 8U;
	const auto source = cpu->vectors[source_register].bytes;
	auto result = (narrow && upper) || (scalar && (cpu->fpcr & 4U) != 0U) ?
		cpu->vectors[destination].bytes :
		std::array<std::byte, 16>{};
	const unsigned source_base = !narrow && upper ? 8U : 0U;
	const unsigned destination_base = narrow && upper ? 8U : 0U;
	std::uint32_t exceptions{};
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto input =
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{source}.subspan(
					source_base + lane * source_bytes, source_bytes));
		const auto converted =
			uml::cpu::arm64::detail::convert_floating_precision(input,
				source_width, destination_width, cpu->fpcr,
				round_odd ? 4U : ~0U);
		uml::cpu::arm64::detail::encode_little_endian(converted.bits,
			std::span{result}.subspan(
				destination_base + lane * destination_bytes,
				destination_bytes));
		exceptions |= converted.exceptions;
	}
	cpu->vectors[destination].bytes = result;
	cpu->fpsr |= exceptions;
}

extern "C" [[gnu::noinline]] void uml_arm64_floating_to_integer_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const unsigned destination = operand & 31U;
	const unsigned source_register = (operand >> 8) & 31U;
	const unsigned shape = (operand >> 16) & 15U;
	const bool unsigned_result = ((operand >> 20) & 1U) != 0U;
	const bool destination_gpr = ((operand >> 21) & 1U) != 0U;
	const auto rounding = static_cast<
		uml::cpu::arm64::detail::FloatingIntegerRounding>(
			(operand >> 24) & 7U);
	constexpr std::array widths{16U, 32U, 64U, 16U, 16U, 32U, 32U, 64U};
	constexpr std::array vector_bytes{2U, 4U, 8U, 8U, 16U, 8U, 16U, 16U};
	const unsigned width = widths[shape];
	const bool cross_width = ((operand >> 27) & 1U) != 0U;
	const unsigned fractional_bits = (operand >> 28) & 127U;
	const unsigned integer_width = destination_gpr || cross_width ?
		(((operand >> 22) & 1U) != 0U ? 64U : 32U) : width;
	const bool scalar = ((operand >> 23) & 1U) != 0U;
	const unsigned bytes = destination_gpr ? width / 8U :
		scalar ? integer_width / 8U : vector_bytes[shape];
	const unsigned lane_bytes = width / 8U;
	const auto source = cpu->vectors[source_register].bytes;
	std::uint32_t exceptions{};
	if (destination_gpr) {
		const auto input =
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{source}.first(lane_bytes));
		const auto converted =
			uml::cpu::arm64::detail::convert_floating_to_integer_bits(
				input, width, integer_width, fractional_bits, unsigned_result,
				rounding, cpu->fpcr);
		cpu->write_register(destination,
			integer_width == 64U ? converted.value :
				static_cast<std::uint32_t>(converted.value), false);
		exceptions = converted.exceptions;
	} else {
		auto result = scalar && (cpu->fpcr & 4U) != 0U ?
			cpu->vectors[destination].bytes : std::array<std::byte, 16>{};
		const unsigned lanes = scalar ? 1U : bytes / lane_bytes;
		for (unsigned lane = 0; lane < lanes; ++lane) {
			const auto input =
				uml::cpu::arm64::detail::decode_little_endian(
					std::span{source}.subspan(lane * lane_bytes, lane_bytes));
			const auto converted =
				uml::cpu::arm64::detail::convert_floating_to_integer_bits(
					input, width, integer_width, fractional_bits,
					unsigned_result,
					rounding, cpu->fpcr);
			uml::cpu::arm64::detail::encode_little_endian(converted.value,
				std::span{result}.subspan(lane * integer_width / 8U,
					integer_width / 8U));
			exceptions |= converted.exceptions;
		}
		cpu->vectors[destination].bytes = result;
	}
	cpu->fpsr |= exceptions;
}

extern "C" [[gnu::noinline]] void
uml_arm64_floating_javascript_convert_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const unsigned destination = operand & 31U;
	const unsigned source_register = (operand >> 8) & 31U;
	const auto input = uml::cpu::arm64::detail::decode_little_endian(
		std::span{cpu->vectors[source_register].bytes}.first(8U));
	const auto converted =
		uml::cpu::arm64::detail::convert_floating_to_javascript_integer(
			input, cpu->fpcr);
	cpu->write_register(destination, converted.value, false);
	cpu->nzcv = converted.sets_z ? 0x4000'0000U : 0U;
	cpu->fpsr |= converted.exceptions;
}

extern "C" [[gnu::noinline]] void uml_arm64_integer_to_floating_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const unsigned destination = operand & 31U;
	const unsigned source_register = (operand >> 8) & 31U;
	const unsigned shape = (operand >> 16) & 7U;
	const bool unsigned_source = ((operand >> 20) & 1U) != 0U;
	const bool source_gpr = ((operand >> 21) & 1U) != 0U;
	const bool scalar = ((operand >> 23) & 1U) != 0U;
	const unsigned destination_code = (operand >> 24) & 3U;
	const unsigned destination_width =
		destination_code == 0U ? 16U :
		destination_code == 1U ? 32U : 64U;
	const bool cross_width = ((operand >> 34) & 1U) != 0U;
	const unsigned integer_width = !source_gpr && !cross_width ?
		destination_width : ((operand >> 22) & 1U) != 0U ? 64U : 32U;
	const unsigned fractional_bits = (operand >> 26) & 127U;
	constexpr std::array vector_bytes{2U, 4U, 8U, 8U, 16U, 8U, 16U, 16U};
	const unsigned bytes = scalar ? destination_width / 8U :
		vector_bytes[shape];
	const unsigned lanes = scalar ? 1U : bytes / (destination_width / 8U);
	const unsigned integer_bytes = integer_width / 8U;
	const unsigned destination_bytes = destination_width / 8U;
	const auto source = source_gpr ? std::array<std::byte, 16>{} :
		cpu->vectors[source_register].bytes;
	const std::uint64_t gpr_source = source_gpr ?
		cpu->read_register(source_register, false) : 0U;
	auto result = scalar && (cpu->fpcr & 4U) != 0U ?
		cpu->vectors[destination].bytes : std::array<std::byte, 16>{};
	std::uint32_t exceptions{};
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto input = source_gpr ? gpr_source :
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{source}.subspan(
					lane * integer_bytes, integer_bytes));
		const auto converted =
			uml::cpu::arm64::detail::convert_integer_to_floating_bits(
				input, integer_width, destination_width, fractional_bits,
				unsigned_source, cpu->fpcr);
		uml::cpu::arm64::detail::encode_little_endian(converted.bits,
			std::span{result}.subspan(
				lane * destination_bytes, destination_bytes));
		exceptions |= converted.exceptions;
	}
	cpu->vectors[destination].bytes = result;
	cpu->fpsr |= exceptions;
}

extern "C" [[gnu::noinline]] void uml_arm64_simd_floating_compare_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	using uml::cpu::arm64::detail::SimdFloatingCompareOperation;
	const unsigned destination = operand & 31U;
	const unsigned left_register = (operand >> 8) & 31U;
	const unsigned right_register = (operand >> 16) & 31U;
	const unsigned shape = (operand >> 24) & 15U;
	const auto operation = static_cast<SimdFloatingCompareOperation>(
		(operand >> 28) & 7U);
	const bool zero = ((operand >> 32) & 1U) != 0U;
	const bool scalar = ((operand >> 33) & 1U) != 0U;
	constexpr std::array widths{16U, 32U, 64U, 16U, 16U, 32U, 32U, 64U};
	constexpr std::array vector_bytes{2U, 4U, 8U, 8U, 16U, 8U, 16U, 16U};
	const unsigned width = widths[shape];
	const unsigned bytes = vector_bytes[shape];
	const unsigned lane_bytes = width / 8U;
	const auto left = cpu->vectors[left_register].bytes;
	const auto right = zero ? std::array<std::byte, 16>{} :
		cpu->vectors[right_register].bytes;
	std::array<std::byte, 16> result{};
	if (scalar && !zero && (cpu->fpcr & 4U) != 0U)
		result = right;
	std::uint32_t exceptions{};
	for (unsigned offset = 0; offset < bytes; offset += lane_bytes) {
		const auto first = uml::cpu::arm64::detail::decode_little_endian(
			std::span{left}.subspan(offset, lane_bytes));
		const auto second = zero ? 0U :
			uml::cpu::arm64::detail::decode_little_endian(
				std::span{right}.subspan(offset, lane_bytes));
		const bool absolute =
			operation == SimdFloatingCompareOperation::absolute_greater_equal ||
			operation == SimdFloatingCompareOperation::absolute_greater;
		const auto sign_mask = std::uint64_t{1} << (width - 1U);
		const bool reverse = operation == SimdFloatingCompareOperation::less ||
			operation == SimdFloatingCompareOperation::less_equal;
		const auto comparison =
			uml::cpu::arm64::detail::compare_floating_bits(
				reverse ? second : absolute ? first & ~sign_mask : first,
				reverse ? first : absolute ? second & ~sign_mask : second, width,
				operation != SimdFloatingCompareOperation::equal, cpu->fpcr);
		exceptions |= comparison.exceptions;
		const bool match = operation == SimdFloatingCompareOperation::equal ?
			comparison.nzcv == 0x6000'0000U :
			operation == SimdFloatingCompareOperation::greater_equal ||
			operation == SimdFloatingCompareOperation::less_equal ||
			operation == SimdFloatingCompareOperation::absolute_greater_equal ?
				comparison.nzcv == 0x6000'0000U ||
				comparison.nzcv == 0x2000'0000U :
				comparison.nzcv == 0x2000'0000U;
		const auto lane = match ? width == 64U ? ~std::uint64_t{0} :
			(std::uint64_t{1} << width) - 1U : 0U;
		uml::cpu::arm64::detail::encode_little_endian(lane,
			std::span{result}.subspan(offset, lane_bytes));
	}
	cpu->vectors[destination].bytes = result;
	cpu->fpsr |= exceptions;
}

