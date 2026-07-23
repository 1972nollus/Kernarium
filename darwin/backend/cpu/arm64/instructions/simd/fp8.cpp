// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/fp8.hpp"
#include "../../detail/instruction_helpers.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::backend::arm64 {

namespace {

[[nodiscard]] std::uint32_t read_lane(
	const std::array<std::byte, 16> &bytes, unsigned offset,
	unsigned width) noexcept
{
	std::uint32_t result = 0;
	for (unsigned i = 0; i < width / 8U; ++i)
		result |= static_cast<std::uint32_t>(
			std::to_integer<unsigned>(bytes[offset + i])) << (i * 8U);
	return result;
}

void write_lane(std::array<std::byte, 16> &bytes, unsigned offset,
	unsigned width, std::uint32_t value) noexcept
{
	for (unsigned i = 0; i < width / 8U; ++i)
		bytes[offset + i] = static_cast<std::byte>(value >> (i * 8U));
}

template<unsigned Count>
[[nodiscard]] std::uint32_t dot_lane(std::uint32_t addend, unsigned width,
	const std::array<std::byte, 16> &first, unsigned first_offset,
	const std::array<std::byte, 16> &second, unsigned second_offset,
	std::uint32_t fpcr, std::uint64_t fpmr) noexcept
{
	std::array<std::uint8_t, Count> left{};
	std::array<std::uint8_t, Count> right{};
	for (unsigned i = 0; i < Count; ++i) {
		left[i] = std::to_integer<std::uint8_t>(first[first_offset + i]);
		right[i] = std::to_integer<std::uint8_t>(second[second_offset + i]);
	}
	return detail::fp8_dot_add(addend, width, left, right, fpcr, fpmr);
}

} // namespace

RunResult Dispatcher::fp8(CpuState &cpu, GuestMemory &,
	std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_fp8(instruction);
	/* The reference runpath validates the entire allocation before reading any
	 * source or destination state. */
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, address, instruction, 0, false};
	const auto first = cpu.vectors[decoded.first].bytes;
	const auto second = cpu.vectors[decoded.second].bytes;
	auto result = cpu.vectors[decoded.destination].bytes;

	switch (decoded.operation) {
	case detail::Fp8Operation::convert_to_half:
	case detail::Fp8Operation::convert_to_bfloat: {
		result.fill(std::byte{});
		const bool bfloat = decoded.operation ==
			detail::Fp8Operation::convert_to_bfloat;
		const unsigned source_offset = decoded.upper ? 8U : 0U;
		for (unsigned lane = 0; lane < 8U; ++lane) {
			const auto converted = detail::fp8_to_binary(
				std::to_integer<std::uint8_t>(first[source_offset + lane]),
				decoded.source2, bfloat, cpu.fpcr, cpu.fpmr);
			write_lane(result, lane * 2U, 16U, converted.bits);
			cpu.fpsr |= converted.exceptions;
		}
		break;
	}
	case detail::Fp8Operation::convert_from_half: {
		result.fill(std::byte{});
		for (unsigned lane = 0; lane < decoded.lanes; ++lane) {
			const auto one = detail::binary_to_fp8(
				read_lane(first, lane * 2U, 16U), 16U,
				cpu.fpcr, cpu.fpmr);
			const auto two = detail::binary_to_fp8(
				read_lane(second, lane * 2U, 16U), 16U,
				cpu.fpcr, cpu.fpmr);
			result[lane] = static_cast<std::byte>(one.bits);
			result[decoded.lanes + lane] = static_cast<std::byte>(two.bits);
			cpu.fpsr |= one.exceptions | two.exceptions;
		}
		break;
	}
	case detail::Fp8Operation::convert_from_single: {
		if (!decoded.upper)
			result.fill(std::byte{});
		const unsigned output = decoded.upper ? 8U : 0U;
		for (unsigned lane = 0; lane < 4U; ++lane) {
			const auto one = detail::binary_to_fp8(
				read_lane(first, lane * 4U, 32U), 32U,
				cpu.fpcr, cpu.fpmr);
			const auto two = detail::binary_to_fp8(
				read_lane(second, lane * 4U, 32U), 32U,
				cpu.fpcr, cpu.fpmr);
			result[output + lane] = static_cast<std::byte>(one.bits);
			result[output + 4U + lane] = static_cast<std::byte>(two.bits);
			cpu.fpsr |= one.exceptions | two.exceptions;
		}
		break;
	}
	case detail::Fp8Operation::dot_half:
	case detail::Fp8Operation::dot_single: {
		const unsigned width = decoded.operation ==
			detail::Fp8Operation::dot_half ? 16U : 32U;
		const unsigned count = width / 8U;
		for (unsigned lane = 0; lane < decoded.lanes; ++lane) {
			const unsigned offset = lane * count;
			const unsigned right = decoded.by_element ?
				decoded.index * count : offset;
			const auto addend = read_lane(result, lane * width / 8U, width);
			const auto value = width == 16U ?
				dot_lane<2>(addend, width, first, offset, second, right,
					cpu.fpcr, cpu.fpmr) :
				dot_lane<4>(addend, width, first, offset, second, right,
					cpu.fpcr, cpu.fpmr);
			write_lane(result, lane * width / 8U, width, value);
		}
		if (decoded.lanes * width == 64U)
			for (unsigned i = 8U; i < 16U; ++i)
				result[i] = std::byte{};
		break;
	}
	case detail::Fp8Operation::multiply_half:
	case detail::Fp8Operation::multiply_single: {
		const unsigned width = decoded.operation ==
			detail::Fp8Operation::multiply_half ? 16U : 32U;
		const unsigned lanes = 128U / width;
		const unsigned stride = width / 8U;
		for (unsigned lane = 0; lane < lanes; ++lane) {
			const unsigned left = lane * stride + decoded.selection;
			const unsigned right = decoded.by_element ? decoded.index : left;
			const auto addend = read_lane(result, lane * stride, width);
			const auto value = dot_lane<1>(addend, width, first, left,
				second, right, cpu.fpcr, cpu.fpmr);
			write_lane(result, lane * stride, width, value);
		}
		break;
	}
	case detail::Fp8Operation::matrix_half:
		for (unsigned segment = 0; segment < 2U; ++segment)
			for (unsigned row = 0; row < 2U; ++row)
				for (unsigned column = 0; column < 2U; ++column) {
					const unsigned output = segment * 8U +
						(row * 2U + column) * 2U;
					const auto value = dot_lane<4>(read_lane(result,
						output, 16U), 16U, first,
						segment * 8U + row * 4U, second,
						segment * 8U + column * 4U,
						cpu.fpcr, cpu.fpmr);
					write_lane(result, output, 16U, value);
				}
		break;
	case detail::Fp8Operation::matrix_single:
		for (unsigned row = 0; row < 2U; ++row)
			for (unsigned column = 0; column < 2U; ++column) {
				const unsigned output = (row * 2U + column) * 4U;
				const auto value = dot_lane<8>(read_lane(result, output,
					32U), 32U, first, row * 8U, second,
					column * 8U, cpu.fpcr, cpu.fpmr);
				write_lane(result, output, 32U, value);
			}
		break;
	}
	cpu.vectors[decoded.destination].bytes = result;
	cpu.pc += 4U;
	return detail::continue_after(address, instruction);
}

} // namespace uml::backend::arm64
