// SPDX-License-Identifier: GPL-2.0-or-later
#include "program.hpp"
#include "software_helpers.hpp"

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
#include "../detail/instruction_primitives.hpp"
#include "../detail/integer_multiply.hpp"
#include "../detail/pointer_auth.hpp"
#include "cpu/host_capabilities.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

extern "C" [[gnu::noinline]] std::uint64_t
uml_arm64_guest_pointer_auth(
	uml::cpu::arm64::CpuState *cpu, std::uint64_t value,
	std::uint64_t modifier, unsigned operation) noexcept
{
	return uml::cpu::arm64::detail::execute_pointer_auth(
		*cpu, value, modifier,
		static_cast<uml::cpu::arm64::detail::PointerAuthOperation>(
			operation));
}

namespace {

using uml::cpu::ArmFeature;
using uml::cpu::host_capabilities;

using uml::cpu::arm64::CpuState;
using uml::cpu::arm64::detail::Fp8Operation;

[[nodiscard]] constexpr std::uintptr_t fp8_vector_offset(
	unsigned reg) noexcept
{
	return offsetof(CpuState, vectors) +
		reg * sizeof(uml::cpu::arm64::VectorRegister);
}

static_assert(fp8_vector_offset(31U) <= 0xffffU);

[[nodiscard]] constexpr std::uintptr_t fp8_operand(
	const uml::cpu::arm64::detail::Fp8Instruction &decoded) noexcept
{
	const bool one_source =
		decoded.operation == Fp8Operation::convert_to_half ||
		decoded.operation == Fp8Operation::convert_to_bfloat;
	return fp8_vector_offset(decoded.destination) |
		(fp8_vector_offset(decoded.first) << 16) |
		((one_source ? 0U : fp8_vector_offset(decoded.second)) << 32) |
		(static_cast<std::uintptr_t>(
			decoded.by_element ? decoded.index : 0U) << 48);
}

extern "C" [[gnu::noinline]] void uml_arm64_gadget_access_fp_state(
	CpuState *cpu) noexcept
{
	cpu->access_fp_state();
}

extern "C" [[gnu::noinline]] std::uint64_t
uml_arm64_simd_rdm_software(CpuState *cpu,
	std::uint32_t instruction) noexcept
{
	/* The builder validates this encoding before selecting the cold software
	 * helper. Keep a defensive result for corrupt/stale program metadata: the
	 * assembly wrapper turns it into the same precise guest illegal event as a
	 * translation-time rejection instead of terminating the host process. */
	return uml::cpu::arm64::detail::
		execute_saturating_rounding_multiply_accumulate(*cpu, instruction);
}

extern "C" [[gnu::noinline]] void
uml_arm64_floating_reciprocal_software(CpuState *cpu,
	std::uintptr_t operand, unsigned descriptor) noexcept
{
	constexpr auto vector_base = offsetof(CpuState, vectors);
	const auto decode_register = [](std::uintptr_t offset) {
		return static_cast<unsigned>(
			(offset - vector_base) / sizeof(uml::cpu::arm64::VectorRegister));
	};
	const auto operation = static_cast<
		uml::cpu::arm64::detail::FloatingReciprocalOperation>(
			descriptor & 7U);
	const bool scalar = (descriptor & 8U) != 0U;
	const unsigned width_code = (descriptor >> 4) & 3U;
	const unsigned width = width_code == 0U ? 16U :
		width_code == 1U ? 32U : 64U;
	const unsigned vector_bytes =
		scalar || (descriptor & 64U) != 0U ? 16U : 8U;
	const bool unary =
		operation != uml::cpu::arm64::detail::
				FloatingReciprocalOperation::reciprocal_step &&
		operation != uml::cpu::arm64::detail::
				FloatingReciprocalOperation::reciprocal_sqrt_step;
	const uml::cpu::arm64::detail::FloatingReciprocal decoded{
		true, true, scalar, unary, width, vector_bytes, operation};
	uml::cpu::arm64::detail::execute_floating_reciprocal_software(
		*cpu, decoded, decode_register(operand & 0xffffU),
		decode_register((operand >> 16) & 0xffffU),
		unary ? 0U : decode_register((operand >> 32) & 0xffffU));
}

[[nodiscard]] std::uint32_t fp8_read_lane(
	const std::array<std::byte, 16> &bytes, unsigned offset,
	unsigned width) noexcept
{
	return static_cast<std::uint32_t>(
		uml::cpu::arm64::detail::decode_little_endian(
			std::span{bytes}.subspan(offset, width / 8U)));
}

void fp8_write_lane(std::array<std::byte, 16> &bytes, unsigned offset,
	unsigned width, std::uint32_t value) noexcept
{
	uml::cpu::arm64::detail::encode_little_endian(value,
		std::span{bytes}.subspan(offset, width / 8U));
}

template<unsigned Count>
[[nodiscard]] std::uint32_t fp8_dot_lane(std::uint32_t addend,
	unsigned width, const std::array<std::byte, 16> &first,
	unsigned first_offset, const std::array<std::byte, 16> &second,
	unsigned second_offset, const CpuState &cpu) noexcept
{
	std::array<std::uint8_t, Count> left{};
	std::array<std::uint8_t, Count> right{};
	for (unsigned i = 0; i < Count; ++i) {
		left[i] = std::to_integer<std::uint8_t>(first[first_offset + i]);
		right[i] = std::to_integer<std::uint8_t>(second[second_offset + i]);
	}
	return uml::cpu::arm64::detail::fp8_dot_add(addend, width,
		left, right, cpu.fpcr, cpu.fpmr);
}

template<Fp8Operation Operation, bool Upper = false, bool Source2 = false,
	bool ByElement = false, unsigned Lanes = 0, unsigned Selection = 0>
[[gnu::noinline]] void fp8_software_transfer(
	CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto destination_offset = operand & 0xffffU;
	const auto first_offset = (operand >> 16) & 0xffffU;
	const auto second_offset = (operand >> 32) & 0xffffU;
	const unsigned index = (operand >> 48) & 15U;
	const auto *base = reinterpret_cast<const std::byte *>(cpu);
	auto *writable_base = reinterpret_cast<std::byte *>(cpu);
	const auto first = reinterpret_cast<
		const uml::cpu::arm64::VectorRegister *>(
			base + first_offset)->bytes;
	const auto second = reinterpret_cast<
		const uml::cpu::arm64::VectorRegister *>(
			base + second_offset)->bytes;
	auto &destination = *reinterpret_cast<
		uml::cpu::arm64::VectorRegister *>(
			writable_base + destination_offset);
	auto result = destination.bytes;

	if constexpr (Operation == Fp8Operation::convert_to_half ||
		Operation == Fp8Operation::convert_to_bfloat) {
		result = {};
		for (unsigned lane = 0; lane < 8U; ++lane) {
			const auto converted = uml::cpu::arm64::detail::fp8_to_binary(
				std::to_integer<std::uint8_t>(first[(Upper ? 8U : 0U) + lane]),
				Source2, Operation == Fp8Operation::convert_to_bfloat,
				cpu->fpcr, cpu->fpmr);
			fp8_write_lane(result, lane * 2U, 16U, converted.bits);
			cpu->fpsr |= converted.exceptions;
		}
	} else if constexpr (Operation == Fp8Operation::convert_from_half) {
		result = {};
		for (unsigned lane = 0; lane < Lanes; ++lane) {
			const auto one = uml::cpu::arm64::detail::binary_to_fp8(
				fp8_read_lane(first, lane * 2U, 16U), 16U,
				cpu->fpcr, cpu->fpmr);
			const auto two = uml::cpu::arm64::detail::binary_to_fp8(
				fp8_read_lane(second, lane * 2U, 16U), 16U,
				cpu->fpcr, cpu->fpmr);
			result[lane] = static_cast<std::byte>(one.bits);
			result[Lanes + lane] = static_cast<std::byte>(two.bits);
			cpu->fpsr |= one.exceptions | two.exceptions;
		}
	} else if constexpr (Operation == Fp8Operation::convert_from_single) {
		if constexpr (!Upper)
			result = {};
		for (unsigned lane = 0; lane < 4U; ++lane) {
			const auto one = uml::cpu::arm64::detail::binary_to_fp8(
				fp8_read_lane(first, lane * 4U, 32U), 32U,
				cpu->fpcr, cpu->fpmr);
			const auto two = uml::cpu::arm64::detail::binary_to_fp8(
				fp8_read_lane(second, lane * 4U, 32U), 32U,
				cpu->fpcr, cpu->fpmr);
			const unsigned output = Upper ? 8U : 0U;
			result[output + lane] = static_cast<std::byte>(one.bits);
			result[output + 4U + lane] = static_cast<std::byte>(two.bits);
			cpu->fpsr |= one.exceptions | two.exceptions;
		}
	} else if constexpr (Operation == Fp8Operation::dot_half ||
		Operation == Fp8Operation::dot_single) {
		constexpr unsigned width = Operation == Fp8Operation::dot_half ? 16U : 32U;
		constexpr unsigned count = width / 8U;
		for (unsigned lane = 0; lane < Lanes; ++lane) {
			const unsigned offset = lane * count;
			const unsigned right_offset = ByElement ? index * count : offset;
			const auto addend = fp8_read_lane(result, lane * count, width);
			const auto value = [&] {
				if constexpr (width == 16U)
					return fp8_dot_lane<2>(addend, width, first, offset,
						second, right_offset, *cpu);
				else
					return fp8_dot_lane<4>(addend, width, first, offset,
						second, right_offset, *cpu);
			}();
			fp8_write_lane(result, lane * count, width, value);
		}
		if constexpr (Lanes * width == 64U)
			std::fill(result.begin() + 8, result.end(), std::byte{});
	} else if constexpr (Operation == Fp8Operation::multiply_half ||
		Operation == Fp8Operation::multiply_single) {
		constexpr unsigned width = Operation == Fp8Operation::multiply_half ?
			16U : 32U;
		constexpr unsigned lanes = 128U / width;
		constexpr unsigned stride = width / 8U;
		for (unsigned lane = 0; lane < lanes; ++lane) {
			const unsigned left_offset = lane * stride + Selection;
			const unsigned right_offset = ByElement ? index : left_offset;
			const auto addend = fp8_read_lane(result, lane * stride, width);
			const auto value = fp8_dot_lane<1>(addend, width, first,
				left_offset, second, right_offset, *cpu);
			fp8_write_lane(result, lane * stride, width, value);
		}
	} else if constexpr (Operation == Fp8Operation::matrix_half) {
		for (unsigned segment = 0; segment < 2U; ++segment)
			for (unsigned row = 0; row < 2U; ++row)
				for (unsigned column = 0; column < 2U; ++column) {
					const unsigned output = segment * 8U +
						(row * 2U + column) * 2U;
					fp8_write_lane(result, output, 16U,
						fp8_dot_lane<4>(fp8_read_lane(result, output, 16U),
							16U, first, segment * 8U + row * 4U,
							second, segment * 8U + column * 4U, *cpu));
				}
	} else if constexpr (Operation == Fp8Operation::matrix_single) {
		for (unsigned row = 0; row < 2U; ++row)
			for (unsigned column = 0; column < 2U; ++column) {
				const unsigned output = (row * 2U + column) * 4U;
				fp8_write_lane(result, output, 32U,
					fp8_dot_lane<8>(fp8_read_lane(result, output, 32U),
						32U, first, row * 8U, second, column * 8U, *cpu));
			}
	}
	destination.bytes = result;
}

using Fp8Software = void (*)(CpuState *, std::uintptr_t) noexcept;

[[nodiscard]] Fp8Software fp8_software_function(
	const uml::cpu::arm64::detail::Fp8Instruction &decoded) noexcept
{
#define UML_FP8_UP(operation, upper, source2) \
	&fp8_software_transfer<Fp8Operation::operation, upper, source2>
	if (decoded.operation == Fp8Operation::convert_to_half) {
		static constexpr Fp8Software table[2][2]{
			{UML_FP8_UP(convert_to_half, false, false),
			 UML_FP8_UP(convert_to_half, false, true)},
			{UML_FP8_UP(convert_to_half, true, false),
			 UML_FP8_UP(convert_to_half, true, true)}};
		return table[decoded.upper][decoded.source2];
	}
	if (decoded.operation == Fp8Operation::convert_to_bfloat) {
		static constexpr Fp8Software table[2][2]{
			{UML_FP8_UP(convert_to_bfloat, false, false),
			 UML_FP8_UP(convert_to_bfloat, false, true)},
			{UML_FP8_UP(convert_to_bfloat, true, false),
			 UML_FP8_UP(convert_to_bfloat, true, true)}};
		return table[decoded.upper][decoded.source2];
	}
#undef UML_FP8_UP
	if (decoded.operation == Fp8Operation::convert_from_half)
		return decoded.lanes == 8U ?
			&fp8_software_transfer<Fp8Operation::convert_from_half,
				false, false, false, 8> :
			&fp8_software_transfer<Fp8Operation::convert_from_half,
				false, false, false, 4>;
	if (decoded.operation == Fp8Operation::convert_from_single)
		return decoded.upper ?
			&fp8_software_transfer<Fp8Operation::convert_from_single, true> :
			&fp8_software_transfer<Fp8Operation::convert_from_single, false>;
#define UML_FP8_DOT(operation, by, lanes) \
	&fp8_software_transfer<Fp8Operation::operation, false, false, by, lanes>
	if (decoded.operation == Fp8Operation::dot_half) {
		static constexpr Fp8Software table[2][2]{
			{UML_FP8_DOT(dot_half, false, 4), UML_FP8_DOT(dot_half, true, 4)},
			{UML_FP8_DOT(dot_half, false, 8), UML_FP8_DOT(dot_half, true, 8)}};
		return table[decoded.lanes == 8U][decoded.by_element];
	}
	if (decoded.operation == Fp8Operation::dot_single) {
		static constexpr Fp8Software table[2][2]{
			{UML_FP8_DOT(dot_single, false, 2), UML_FP8_DOT(dot_single, true, 2)},
			{UML_FP8_DOT(dot_single, false, 4), UML_FP8_DOT(dot_single, true, 4)}};
		return table[decoded.lanes == 4U][decoded.by_element];
	}
#undef UML_FP8_DOT
#define UML_FP8_MUL(operation, by, selection) \
	&fp8_software_transfer<Fp8Operation::operation, false, false, by, 0, selection>
	if (decoded.operation == Fp8Operation::multiply_half) {
		static constexpr Fp8Software table[2][2]{
			{UML_FP8_MUL(multiply_half, false, 0),
			 UML_FP8_MUL(multiply_half, true, 0)},
			{UML_FP8_MUL(multiply_half, false, 1),
			 UML_FP8_MUL(multiply_half, true, 1)}};
		return table[decoded.selection][decoded.by_element];
	}
	if (decoded.operation == Fp8Operation::multiply_single) {
		static constexpr Fp8Software table[4][2]{
			{UML_FP8_MUL(multiply_single, false, 0),
			 UML_FP8_MUL(multiply_single, true, 0)},
			{UML_FP8_MUL(multiply_single, false, 1),
			 UML_FP8_MUL(multiply_single, true, 1)},
			{UML_FP8_MUL(multiply_single, false, 2),
			 UML_FP8_MUL(multiply_single, true, 2)},
			{UML_FP8_MUL(multiply_single, false, 3),
			 UML_FP8_MUL(multiply_single, true, 3)}};
		return table[decoded.selection][decoded.by_element];
	}
#undef UML_FP8_MUL
	if (decoded.operation == Fp8Operation::matrix_half)
		return &fp8_software_transfer<Fp8Operation::matrix_half>;
	return &fp8_software_transfer<Fp8Operation::matrix_single>;
}

} // namespace

namespace {

template<typename Integer,
	uml::cpu::arm64::detail::AtomicFloatingOperation Operation,
	unsigned Width, bool BFloat, bool Acquire, bool Release, bool ReturnOld>
void atomic_floating_transfer(uml::cpu::arm64::CpuState *cpu,
	std::byte *pointer, std::uintptr_t operand) noexcept
{
	using namespace uml::cpu::arm64;
	const unsigned source = operand & 31U;
	const auto source_bits = detail::decode_little_endian(
		std::span{cpu->vectors[source].bytes}.first(sizeof(Integer)));
	auto &location = *reinterpret_cast<Integer *>(pointer);
	auto reference = std::atomic_ref<Integer>{location};
	constexpr auto order = Acquire && Release ? std::memory_order_acq_rel :
		Acquire ? std::memory_order_acquire : Release ?
		std::memory_order_release : std::memory_order_relaxed;
	constexpr auto failure_order = Acquire ? std::memory_order_acquire :
		std::memory_order_relaxed;
	auto expected = reference.load(std::memory_order_relaxed);
	for (;;) {
		const auto observed = expected;
		const auto desired = static_cast<Integer>(
			detail::atomic_floating_result<Operation>(observed, source_bits,
				Width, BFloat, cpu->fpcr));
		if (!reference.compare_exchange_weak(
				expected, desired, order, failure_order))
			continue;
		if constexpr (ReturnOld) {
			const unsigned destination = (operand >> 8) & 31U;
			cpu->vectors[destination].bytes = {};
			detail::encode_little_endian(observed,
				std::span{cpu->vectors[destination].bytes}.first(
					sizeof(Integer)));
		}
		return;
	}
}

} // namespace

#define UML_ATOMIC_FLOATING_TRANSFER_WRAPPER(size_name, integer, width, bfloat, \
	operation_name, operation, variant, acquire, release, return_old) \
extern "C" [[gnu::noinline]] void \
uml_arm64_atomic_floating_transfer_##size_name##_##operation_name##_##variant( \
	uml::cpu::arm64::CpuState *cpu, std::byte *pointer, \
	std::uintptr_t operand) noexcept \
{ \
	atomic_floating_transfer<integer, \
		uml::cpu::arm64::detail::AtomicFloatingOperation::operation, width, \
		bfloat, acquire, release, return_old>(cpu, pointer, operand); \
}

#define UML_ATOMIC_FLOATING_TRANSFER_VARIANTS(size_name, integer, width, \
	bfloat, operation_name, operation) \
	UML_ATOMIC_FLOATING_TRANSFER_WRAPPER(size_name, integer, width, bfloat, \
		operation_name, operation, load_relaxed, false, false, true) \
	UML_ATOMIC_FLOATING_TRANSFER_WRAPPER(size_name, integer, width, bfloat, \
		operation_name, operation, load_release, false, true, true) \
	UML_ATOMIC_FLOATING_TRANSFER_WRAPPER(size_name, integer, width, bfloat, \
		operation_name, operation, load_acquire, true, false, true) \
	UML_ATOMIC_FLOATING_TRANSFER_WRAPPER(size_name, integer, width, bfloat, \
		operation_name, operation, load_acquire_release, true, true, true) \
	UML_ATOMIC_FLOATING_TRANSFER_WRAPPER(size_name, integer, width, bfloat, \
		operation_name, operation, store_relaxed, false, false, false) \
	UML_ATOMIC_FLOATING_TRANSFER_WRAPPER(size_name, integer, width, bfloat, \
		operation_name, operation, store_release, false, true, false)

#define UML_ATOMIC_FLOATING_TRANSFER_OPERATIONS(size_name, integer, width, \
	bfloat) \
	UML_ATOMIC_FLOATING_TRANSFER_VARIANTS(size_name, integer, width, bfloat, \
		add, add) \
	UML_ATOMIC_FLOATING_TRANSFER_VARIANTS(size_name, integer, width, bfloat, \
		max, maximum) \
	UML_ATOMIC_FLOATING_TRANSFER_VARIANTS(size_name, integer, width, bfloat, \
		maxnm, maximum_number) \
	UML_ATOMIC_FLOATING_TRANSFER_VARIANTS(size_name, integer, width, bfloat, \
		min, minimum) \
	UML_ATOMIC_FLOATING_TRANSFER_VARIANTS(size_name, integer, width, bfloat, \
		minnm, minimum_number)

UML_ATOMIC_FLOATING_TRANSFER_OPERATIONS(bf, std::uint16_t, 16U, true)
UML_ATOMIC_FLOATING_TRANSFER_OPERATIONS(h, std::uint16_t, 16U, false)
UML_ATOMIC_FLOATING_TRANSFER_OPERATIONS(s, std::uint32_t, 32U, false)
UML_ATOMIC_FLOATING_TRANSFER_OPERATIONS(d, std::uint64_t, 64U, false)

#undef UML_ATOMIC_FLOATING_TRANSFER_OPERATIONS
#undef UML_ATOMIC_FLOATING_TRANSFER_VARIANTS
#undef UML_ATOMIC_FLOATING_TRANSFER_WRAPPER

extern "C" [[gnu::noinline]] void uml_arm64_simd_lut_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto vector_at = [cpu, operand](unsigned shift)
		-> const uml::cpu::arm64::VectorRegister & {
		const auto offset = (operand >> shift) & 0x7ffU;
		return *reinterpret_cast<const uml::cpu::arm64::VectorRegister *>(
			reinterpret_cast<const std::byte *>(cpu) + offset);
	};
	const auto vector_at_mutable = [cpu, operand](unsigned shift)
		-> uml::cpu::arm64::VectorRegister & {
		const auto offset = (operand >> shift) & 0x7ffU;
		return *reinterpret_cast<uml::cpu::arm64::VectorRegister *>(
			reinterpret_cast<std::byte *>(cpu) + offset);
	};
	const unsigned shape = (operand >> 44) & 3U;
	const unsigned part = (operand >> 46) & 7U;
	const unsigned index_bits = shape < 2U ? 2U : 4U;
	const unsigned element_bytes = (shape & 1U) != 0U ? 2U : 1U;
	const unsigned elements = 16U / element_bytes;
	const auto indices = vector_at(33).bytes;
	const auto table1 = vector_at(11).bytes;
	const auto table2 = vector_at(22).bytes;
	std::array<std::byte, 16> result{};
	const unsigned mask = (1U << index_bits) - 1U;
	const unsigned first_index = elements * part;
	for (unsigned element = 0; element < elements; ++element) {
		const unsigned bit = (first_index + element) * index_bits;
		const unsigned index =
			(std::to_integer<unsigned>(indices[bit / 8U]) >> (bit & 7U)) &
			mask;
		const bool second = shape == 3U && index >= elements;
		const auto &table = second ? table2 : table1;
		const unsigned table_index = second ? index - elements : index;
		std::copy_n(table.begin() + table_index * element_bytes,
			element_bytes, result.begin() + element * element_bytes);
	}
	vector_at_mutable(0).bytes = result;
}

extern "C" [[gnu::noinline]] void uml_arm64_floating_round_sized_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const unsigned destination = operand & 31U;
	const unsigned source_register = (operand >> 8) & 31U;
	const unsigned width = (operand & (std::uintptr_t{1} << 16)) != 0U ?
		64U : 32U;
	const unsigned integer_bits =
		(operand & (std::uintptr_t{1} << 17)) != 0U ? 64U : 32U;
	const bool current = (operand & (std::uintptr_t{1} << 18)) != 0U;
	const bool scalar = (operand & (std::uintptr_t{1} << 19)) != 0U;
	const unsigned vector_bytes = scalar ? width / 8U :
		(operand & (std::uintptr_t{1} << 20)) != 0U ? 16U : 8U;
	const auto source = cpu->vectors[source_register].bytes;
	auto result = scalar && (cpu->fpcr & 4U) != 0U ?
		cpu->vectors[destination].bytes : std::array<std::byte, 16>{};
	const unsigned lane_bytes = width / 8U;
	const unsigned lanes = scalar ? 1U : vector_bytes / lane_bytes;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto raw = uml::cpu::arm64::detail::decode_little_endian(
			std::span{source}.subspan(lane * lane_bytes, lane_bytes));
		const auto rounded = width == 32U ?
			uml::cpu::arm64::detail::round_floating_sized<float>(
				raw, integer_bits, current) :
			uml::cpu::arm64::detail::round_floating_sized<double>(
				raw, integer_bits, current);
		uml::cpu::arm64::detail::encode_little_endian(rounded.bits,
			std::span{result}.subspan(lane * lane_bytes, lane_bytes));
		cpu->fpsr |= rounded.exceptions;
	}
	cpu->vectors[destination].bytes = result;
}

namespace uml::cpu::arm64::gadgets::software {

std::uintptr_t fp8_software_operand(
	const arm64::detail::Fp8Instruction &decoded) noexcept
{
	return ::fp8_operand(decoded);
}

Fp8Software fp8_software_function_for(
	const arm64::detail::Fp8Instruction &decoded) noexcept
{
	return ::fp8_software_function(decoded);
}

} // namespace uml::cpu::arm64::gadgets::software
