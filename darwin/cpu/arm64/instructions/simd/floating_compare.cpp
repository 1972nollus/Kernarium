// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/bfloat_dot.hpp"
#include "../../detail/bfloat_matrix.hpp"
#include "../../detail/bfloat_multiply.hpp"
#include "../../detail/half_fused_long.hpp"
#include "../../detail/half_dot.hpp"
#include "../../detail/half_matrix.hpp"
#include "../../detail/floating_arithmetic.hpp"
#include "../../detail/bfloat_conversion.hpp"
#include "../../detail/floating_compare.hpp"
#include "../../detail/floating_complex.hpp"
#include "../../detail/floating_conversion.hpp"
#include "../../detail/floating_fused.hpp"
#include "../../detail/floating_horizontal.hpp"
#include "../../detail/floating_reciprocal.hpp"
#include "../../detail/floating_reciprocal_software.hpp"
#include "../../detail/floating_round.hpp"
#include "../../detail/floating_scale.hpp"
#include "../../detail/instruction_helpers.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace uml::cpu::arm64 {

using detail::continue_after;
using detail::decode_little_endian;
using detail::encode_little_endian;

RunResult Dispatcher::simd_floating_compare(CpuState &cpu, GuestMemory &,
					     std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const auto decoded = detail::decode_simd_floating_compare(instruction);
	if (!decoded.recognized || !decoded.valid)
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
	const unsigned lane_bytes = decoded.width / 8U;
	const unsigned lanes = decoded.vector_bytes / lane_bytes;
	const auto left = cpu.vectors[(instruction >> 5) & 31U].bytes;
	const auto right = decoded.zero ? std::array<std::byte, 16>{} :
		cpu.vectors[(instruction >> 16) & 31U].bytes;
	std::array<std::byte, 16> result{};
	if (decoded.scalar && !decoded.zero && (cpu.fpcr & 4U) != 0U)
		result = right;
	std::uint32_t exceptions{};
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto first = decode_little_endian(std::span{left}.subspan(
			lane * lane_bytes, lane_bytes));
		const auto second = decoded.zero ? 0U : decode_little_endian(
			std::span{right}.subspan(lane * lane_bytes, lane_bytes));
		const bool absolute = decoded.operation ==
			detail::SimdFloatingCompareOperation::absolute_greater_equal ||
			decoded.operation ==
			detail::SimdFloatingCompareOperation::absolute_greater;
		const auto sign_mask = std::uint64_t{1} << (decoded.width - 1U);
		const bool reverse =
			decoded.operation == detail::SimdFloatingCompareOperation::less_equal ||
			decoded.operation == detail::SimdFloatingCompareOperation::less;
		const auto comparison = detail::compare_floating_bits(
			reverse ? second : absolute ? first & ~sign_mask : first,
			reverse ? first : absolute ? second & ~sign_mask : second,
			decoded.width,
			decoded.operation != detail::SimdFloatingCompareOperation::equal,
			cpu.fpcr);
		exceptions |= comparison.exceptions;
		const bool match =
			decoded.operation == detail::SimdFloatingCompareOperation::equal ?
				comparison.nzcv == 0x6000'0000U :
			decoded.operation == detail::SimdFloatingCompareOperation::greater_equal ||
			decoded.operation == detail::SimdFloatingCompareOperation::less_equal ||
			decoded.operation ==
				detail::SimdFloatingCompareOperation::absolute_greater_equal ?
				comparison.nzcv == 0x6000'0000U ||
				comparison.nzcv == 0x2000'0000U :
				comparison.nzcv == 0x2000'0000U;
		encode_little_endian(match ?
			(decoded.width == 64U ? ~std::uint64_t{0} :
				(std::uint64_t{1} << decoded.width) - 1U) : 0U,
			std::span{result}.subspan(lane * lane_bytes, lane_bytes));
	}
	cpu.vectors[instruction & 31U].bytes = result;
	cpu.fpsr |= exceptions;
	cpu.pc += 4;
	return continue_after(address, instruction);
}


} // namespace uml::cpu::arm64
