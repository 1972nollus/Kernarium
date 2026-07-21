// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/instruction_helpers.hpp"

#include <arm_neon.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace uml::backend::arm64 {

namespace {

template<typename Vector>
[[nodiscard]] Vector load_vector(const VectorRegister &source)
{
	static_assert(sizeof(Vector) == sizeof(source.bytes));
	Vector value;
	std::memcpy(&value, source.bytes.data(), sizeof(value));
	return value;
}

template<typename Vector>
void store_vector(VectorRegister &destination, Vector value)
{
	static_assert(sizeof(Vector) == sizeof(destination.bytes));
	std::memcpy(destination.bytes.data(), &value, sizeof(value));
}

void store_word(VectorRegister &destination, std::uint32_t value)
{
	destination.bytes.fill(std::byte{});
	detail::encode_little_endian(value,
		std::span{destination.bytes}.first(sizeof(value)));
}

} // namespace

/* Darwin's arm64 targets provide the Armv8 cryptographic extensions.
 * Execute the guest primitive directly on the host so crypto-heavy userspace
 * does not pay for a software S-box or four scalar hash rounds per instruction.
 */
__attribute__((target("aes,sha2,sha3,neon")))
RunResult Dispatcher::simd_crypto(CpuState &cpu, GuestMemory &,
				  std::uint32_t instruction) const
{
	const auto address = cpu.pc;
	const unsigned destination_index = instruction & 0x1fU;
	const unsigned first_index = (instruction >> 5) & 0x1fU;
	const unsigned second_index = (instruction >> 16) & 0x1fU;
	auto &destination = cpu.vectors[destination_index];

	if ((instruction & 0xffff'8c00U) == 0x4e28'0800U) {
		const auto source = load_vector<uint8x16_t>(cpu.vectors[first_index]);
		const auto old_destination = load_vector<uint8x16_t>(destination);
		uint8x16_t result;
		switch ((instruction >> 12) & 0x7U) {
		case 4: result = vaeseq_u8(old_destination, source); break;
		case 5: result = vaesdq_u8(old_destination, source); break;
		case 6: result = vaesmcq_u8(source); break;
		case 7: result = vaesimcq_u8(source); break;
		default:
			return {StopReason::unsupported_instruction, address,
				instruction, 0, false};
		}
		store_vector(destination, result);
	} else if ((instruction & 0xbf20'fc00U) == 0x0e20'e000U) {
		const unsigned size = (instruction >> 22) & 0x3U;
		const std::size_t source_offset =
			(instruction & 0x4000'0000U) != 0 ? 8 : 0;
		const auto &left = cpu.vectors[first_index].bytes;
		const auto &right = cpu.vectors[second_index].bytes;
		if (size == 3) {
			const auto lhs = static_cast<poly64_t>(
				detail::decode_little_endian(
					std::span{left}.subspan(source_offset, 8)));
			const auto rhs = static_cast<poly64_t>(
				detail::decode_little_endian(
					std::span{right}.subspan(source_offset, 8)));
			store_vector(destination, vmull_p64(lhs, rhs));
		} else if (size == 0) {
			std::array<std::byte, 16> result{};
			for (std::size_t lane = 0; lane < 8; ++lane) {
				const auto lhs = std::to_integer<std::uint16_t>(
					left[source_offset + lane]);
				const auto rhs = std::to_integer<unsigned>(
					right[source_offset + lane]);
				std::uint16_t product = 0;
				for (unsigned bit = 0; bit < 8; ++bit)
					if ((rhs & (1U << bit)) != 0)
						product ^= static_cast<std::uint16_t>(lhs << bit);
				detail::encode_little_endian(product,
					std::span{result}.subspan(lane * 2, 2));
			}
			destination.bytes = result;
		} else {
			return {StopReason::unsupported_instruction, address,
				instruction, 0, false};
		}
	} else if ((instruction & 0xffe0'8c00U) == 0x5e00'0000U) {
		const auto first = load_vector<uint32x4_t>(cpu.vectors[first_index]);
		const auto second = load_vector<uint32x4_t>(cpu.vectors[second_index]);
		const auto old_destination = load_vector<uint32x4_t>(destination);
		uint32x4_t result;
		switch ((instruction >> 12) & 0x7U) {
		case 0:
			result = vsha1cq_u32(old_destination,
				vgetq_lane_u32(first, 0), second);
			break;
		case 1:
			result = vsha1pq_u32(old_destination,
				vgetq_lane_u32(first, 0), second);
			break;
		case 2:
			result = vsha1mq_u32(old_destination,
				vgetq_lane_u32(first, 0), second);
			break;
		case 3:
			result = vsha1su0q_u32(old_destination, first, second);
			break;
		case 4:
			result = vsha256hq_u32(old_destination, first, second);
			break;
		case 5:
			result = vsha256h2q_u32(old_destination, first, second);
			break;
		case 6:
			result = vsha256su1q_u32(old_destination, first, second);
			break;
		default:
			return {StopReason::unsupported_instruction, address,
				instruction, 0, false};
		}
		store_vector(destination, result);
	} else if ((instruction & 0xffff'8c00U) == 0x5e28'0800U) {
		const auto source = load_vector<uint32x4_t>(cpu.vectors[first_index]);
		switch ((instruction >> 12) & 0x3U) {
		case 0:
			store_word(destination, vsha1h_u32(vgetq_lane_u32(source, 0)));
			break;
		case 1:
			store_vector(destination, vsha1su1q_u32(
				load_vector<uint32x4_t>(destination), source));
			break;
		case 2:
			store_vector(destination, vsha256su0q_u32(
				load_vector<uint32x4_t>(destination), source));
			break;
		default:
			return {StopReason::unsupported_instruction, address,
				instruction, 0, false};
		}
	} else if ((instruction & 0xffe0'fc00U) == 0xce60'8000U ||
		   (instruction & 0xffe0'fc00U) == 0xce60'8400U ||
		   (instruction & 0xffe0'fc00U) == 0xce60'8800U) {
		const auto first = load_vector<uint64x2_t>(cpu.vectors[first_index]);
		const auto second = load_vector<uint64x2_t>(cpu.vectors[second_index]);
		const auto old_destination = load_vector<uint64x2_t>(destination);
		const auto operation = instruction & 0xffe0'fc00U;
		if (operation == 0xce60'8000U)
			store_vector(destination,
				vsha512hq_u64(old_destination, first, second));
		else if (operation == 0xce60'8400U)
			store_vector(destination,
				vsha512h2q_u64(old_destination, first, second));
		else
			store_vector(destination,
				vsha512su1q_u64(old_destination, first, second));
	} else if ((instruction & 0xffff'fc00U) == 0xcec0'8000U) {
		store_vector(destination, vsha512su0q_u64(
			load_vector<uint64x2_t>(destination),
			load_vector<uint64x2_t>(cpu.vectors[first_index])));
	} else if ((instruction & 0xffe0'8000U) == 0xce00'0000U ||
		   (instruction & 0xffe0'8000U) == 0xce20'0000U) {
		const unsigned third_index = (instruction >> 10) & 0x1fU;
		const auto first = load_vector<uint64x2_t>(cpu.vectors[first_index]);
		const auto second = load_vector<uint64x2_t>(cpu.vectors[second_index]);
		const auto third = load_vector<uint64x2_t>(cpu.vectors[third_index]);
		store_vector(destination,
			(instruction & 0x0020'0000U) == 0 ?
				veor3q_u64(first, second, third) :
				vbcaxq_u64(first, second, third));
	} else if ((instruction & 0xffe0'fc00U) == 0xce60'8c00U) {
		store_vector(destination, vrax1q_u64(
			load_vector<uint64x2_t>(cpu.vectors[first_index]),
			load_vector<uint64x2_t>(cpu.vectors[second_index])));
	} else if ((instruction & 0xffe0'0000U) == 0xce80'0000U) {
		const unsigned rotate = (instruction >> 10) & 0x3fU;
		const auto &first = cpu.vectors[first_index].bytes;
		const auto &second = cpu.vectors[second_index].bytes;
		std::array<std::byte, 16> result{};
		for (std::size_t lane = 0; lane < 2; ++lane) {
			const auto value = detail::decode_little_endian(
				std::span{first}.subspan(lane * 8, 8)) ^
				detail::decode_little_endian(
					std::span{second}.subspan(lane * 8, 8));
			detail::encode_little_endian(std::rotr(value, rotate),
				std::span{result}.subspan(lane * 8, 8));
		}
		destination.bytes = result;
	} else {
		return {StopReason::unsupported_instruction, address, instruction, 0,
			false};
	}

	cpu.pc += 4;
	return detail::continue_after(address, instruction);
}

} // namespace uml::backend::arm64
