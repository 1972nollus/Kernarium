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

[[nodiscard]] std::array<std::uint32_t, 4> load_words(
	const VectorRegister &source)
{
	std::array<std::uint32_t, 4> result{};
	for (std::size_t lane = 0; lane < result.size(); ++lane)
		result[lane] = static_cast<std::uint32_t>(
			detail::decode_little_endian(
				std::span{source.bytes}.subspan(lane * 4, 4)));
	return result;
}

void store_words(VectorRegister &destination,
	const std::array<std::uint32_t, 4> &words)
{
	for (std::size_t lane = 0; lane < words.size(); ++lane)
		detail::encode_little_endian(words[lane],
			std::span{destination.bytes}.subspan(lane * 4, 4));
}

[[nodiscard]] std::array<std::uint64_t, 2> load_doublewords(
	const VectorRegister &source)
{
	return {detail::decode_little_endian(
			std::span{source.bytes}.first<8>()),
		detail::decode_little_endian(
			std::span{source.bytes}.subspan<8, 8>())};
}

void store_doublewords(VectorRegister &destination,
	const std::array<std::uint64_t, 2> &words)
{
	detail::encode_little_endian(words[0],
		std::span{destination.bytes}.first<8>());
	detail::encode_little_endian(words[1],
		std::span{destination.bytes}.subspan<8, 8>());
}

[[nodiscard]] constexpr std::uint32_t sha_choose(
	std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
	return ((y ^ z) & x) ^ z;
}

[[nodiscard]] constexpr std::uint32_t sha_majority(
	std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
	return (x & y) | ((x | y) & z);
}

[[nodiscard]] std::array<std::uint32_t, 4> sha1_hash(
	std::array<std::uint32_t, 4> x, std::uint32_t y,
	const std::array<std::uint32_t, 4> &words, unsigned operation)
{
	for (std::size_t round = 0; round < 4; ++round) {
		const std::uint32_t function = operation == 0U ?
			sha_choose(x[1], x[2], x[3]) : operation == 1U ?
			(x[1] ^ x[2] ^ x[3]) : sha_majority(x[1], x[2], x[3]);
		const std::uint32_t next = y + std::rotl(x[0], 5) + function +
			words[round];
		x[1] = std::rotl(x[1], 30);
		y = x[3];
		x = {next, x[0], x[1], x[2]};
	}
	return x;
}

[[nodiscard]] std::array<std::uint32_t, 4> sha256_hash(
	std::array<std::uint32_t, 4> x,
	std::array<std::uint32_t, 4> y,
	const std::array<std::uint32_t, 4> &words, bool part1)
{
	for (std::size_t round = 0; round < 4; ++round) {
		const auto sigma0 = std::rotr(x[0], 2) ^ std::rotr(x[0], 13) ^
			std::rotr(x[0], 22);
		const auto sigma1 = std::rotr(y[0], 6) ^ std::rotr(y[0], 11) ^
			std::rotr(y[0], 25);
		const std::uint32_t temporary = y[3] + sigma1 +
			sha_choose(y[0], y[1], y[2]) + words[round];
		x[3] += temporary;
		y[3] = temporary + sigma0 + sha_majority(x[0], x[1], x[2]);
		const auto old_x = x;
		const auto old_y = y;
		x = {old_y[3], old_x[0], old_x[1], old_x[2]};
		y = {old_x[3], old_y[0], old_y[1], old_y[2]};
	}
	return part1 ? x : y;
}

[[nodiscard]] std::uint32_t sm4_substitute(std::uint32_t value)
{
	static constexpr std::array<std::uint8_t, 256> sbox{
		0xd6,0x90,0xe9,0xfe,0xcc,0xe1,0x3d,0xb7,0x16,0xb6,0x14,0xc2,0x28,0xfb,0x2c,0x05,
		0x2b,0x67,0x9a,0x76,0x2a,0xbe,0x04,0xc3,0xaa,0x44,0x13,0x26,0x49,0x86,0x06,0x99,
		0x9c,0x42,0x50,0xf4,0x91,0xef,0x98,0x7a,0x33,0x54,0x0b,0x43,0xed,0xcf,0xac,0x62,
		0xe4,0xb3,0x1c,0xa9,0xc9,0x08,0xe8,0x95,0x80,0xdf,0x94,0xfa,0x75,0x8f,0x3f,0xa6,
		0x47,0x07,0xa7,0xfc,0xf3,0x73,0x17,0xba,0x83,0x59,0x3c,0x19,0xe6,0x85,0x4f,0xa8,
		0x68,0x6b,0x81,0xb2,0x71,0x64,0xda,0x8b,0xf8,0xeb,0x0f,0x4b,0x70,0x56,0x9d,0x35,
		0x1e,0x24,0x0e,0x5e,0x63,0x58,0xd1,0xa2,0x25,0x22,0x7c,0x3b,0x01,0x21,0x78,0x87,
		0xd4,0x00,0x46,0x57,0x9f,0xd3,0x27,0x52,0x4c,0x36,0x02,0xe7,0xa0,0xc4,0xc8,0x9e,
		0xea,0xbf,0x8a,0xd2,0x40,0xc7,0x38,0xb5,0xa3,0xf7,0xf2,0xce,0xf9,0x61,0x15,0xa1,
		0xe0,0xae,0x5d,0xa4,0x9b,0x34,0x1a,0x55,0xad,0x93,0x32,0x30,0xf5,0x8c,0xb1,0xe3,
		0x1d,0xf6,0xe2,0x2e,0x82,0x66,0xca,0x60,0xc0,0x29,0x23,0xab,0x0d,0x53,0x4e,0x6f,
		0xd5,0xdb,0x37,0x45,0xde,0xfd,0x8e,0x2f,0x03,0xff,0x6a,0x72,0x6d,0x6c,0x5b,0x51,
		0x8d,0x1b,0xaf,0x92,0xbb,0xdd,0xbc,0x7f,0x11,0xd9,0x5c,0x41,0x1f,0x10,0x5a,0xd8,
		0x0a,0xc1,0x31,0x88,0xa5,0xcd,0x7b,0xbd,0x2d,0x74,0xd0,0x12,0xb8,0xe5,0xb4,0xb0,
		0x89,0x69,0x97,0x4a,0x0c,0x96,0x77,0x7e,0x65,0xb9,0xf1,0x09,0xc5,0x6e,0xc6,0x84,
		0x18,0xf0,0x7d,0xec,0x3a,0xdc,0x4d,0x20,0x79,0xee,0x5f,0x3e,0xd7,0xcb,0x39,0x48};
	std::uint32_t result = 0;
	for (unsigned byte = 0; byte < 4; ++byte)
		result |= static_cast<std::uint32_t>(
			sbox[(value >> (byte * 8U)) & 0xffU]) << (byte * 8U);
	return result;
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
		const unsigned operation = (instruction >> 12) & 0x7U;
		if (operation < 4U)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
		const auto source = load_vector<uint8x16_t>(cpu.vectors[first_index]);
		const auto old_destination = load_vector<uint8x16_t>(destination);
		uint8x16_t result;
		switch (operation) {
		case 4: result = vaeseq_u8(old_destination, source); break;
		case 5: result = vaesdq_u8(old_destination, source); break;
		case 6: result = vaesmcq_u8(source); break;
		case 7: result = vaesimcq_u8(source); break;
		default: __builtin_unreachable();
		}
		store_vector(destination, result);
	} else if ((instruction & 0xbf20'fc00U) == 0x0e20'e000U) {
		const unsigned size = (instruction >> 22) & 0x3U;
		if (size != 0U && size != 3U)
			return {StopReason::illegal_instruction, address, instruction, 0,
				false};
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
		}
	} else if ((instruction & 0xffe0'8c00U) == 0x5e00'0000U) {
		const auto first = load_words(cpu.vectors[first_index]);
		const auto second = load_words(cpu.vectors[second_index]);
		const auto old_destination = load_words(destination);
		std::array<std::uint32_t, 4> result{};
		switch ((instruction >> 12) & 0x7U) {
		case 0: result = sha1_hash(old_destination, first[0], second, 0); break;
		case 1: result = sha1_hash(old_destination, first[0], second, 1); break;
		case 2: result = sha1_hash(old_destination, first[0], second, 2); break;
		case 3:
			result = {second[0] ^ old_destination[0] ^ old_destination[2],
				second[1] ^ old_destination[1] ^ old_destination[3],
				second[2] ^ old_destination[2] ^ first[0],
				second[3] ^ old_destination[3] ^ first[1]};
			break;
		case 4: result = sha256_hash(old_destination, first, second, true); break;
		case 5: result = sha256_hash(first, old_destination, second, false); break;
		case 6: {
			const std::array temporary{first[1], first[2], first[3], second[0]};
			for (std::size_t lane = 0; lane < 2; ++lane) {
				const auto value = second[lane + 2];
				result[lane] = (std::rotr(value, 17) ^ std::rotr(value, 19) ^
					(value >> 10)) + old_destination[lane] + temporary[lane];
			}
			for (std::size_t lane = 2; lane < 4; ++lane) {
				const auto value = result[lane - 2];
				result[lane] = (std::rotr(value, 17) ^ std::rotr(value, 19) ^
					(value >> 10)) + old_destination[lane] + temporary[lane];
			}
			break;
		}
		default:
			return {StopReason::illegal_instruction, address,
				instruction, 0, false};
		}
		store_words(destination, result);
	} else if ((instruction & 0xffff'8c00U) == 0x5e28'0800U) {
		const auto source = load_words(cpu.vectors[first_index]);
		switch ((instruction >> 12) & 0x3U) {
		case 0:
			store_word(destination, std::rotl(source[0], 30));
			break;
		case 1: {
			const auto old_destination = load_words(destination);
			const std::array temporary{
				old_destination[0] ^ source[1],
				old_destination[1] ^ source[2],
				old_destination[2] ^ source[3],
				old_destination[3]};
			std::array result{
				std::rotl(temporary[0], 1),
				std::rotl(temporary[1], 1),
				std::rotl(temporary[2], 1),
				std::rotl(temporary[3], 1) ^ std::rotl(temporary[0], 2)};
			store_words(destination, result);
			break;
		}
		case 2: {
			const auto old_destination = load_words(destination);
			const std::array temporary{old_destination[1], old_destination[2],
				old_destination[3], source[0]};
			std::array<std::uint32_t, 4> result{};
			for (std::size_t lane = 0; lane < 4; ++lane) {
				const auto value = temporary[lane];
				result[lane] = old_destination[lane] + (std::rotr(value, 7) ^
					std::rotr(value, 18) ^ (value >> 3));
			}
			store_words(destination, result);
			break;
		}
		default:
			return {StopReason::illegal_instruction, address,
				instruction, 0, false};
		}
	} else if ((instruction & 0xffe0'fc00U) == 0xce60'8000U ||
		   (instruction & 0xffe0'fc00U) == 0xce60'8400U ||
		   (instruction & 0xffe0'fc00U) == 0xce60'8800U) {
		const auto first = load_doublewords(cpu.vectors[first_index]);
		const auto second = load_doublewords(cpu.vectors[second_index]);
		const auto old_destination = load_doublewords(destination);
		const auto operation = instruction & 0xffe0'fc00U;
		std::array<std::uint64_t, 2> result{};
		if (operation == 0xce60'8000U) {
			const auto sigma1 = [](std::uint64_t value) {
				return std::rotr(value, 14) ^ std::rotr(value, 18) ^
					std::rotr(value, 41);
			};
			result[1] = (second[1] & first[0]) ^ (~second[1] & first[1]);
			result[1] += sigma1(second[1]) + old_destination[1];
			const auto temporary = result[1] + second[0];
			result[0] = (temporary & second[1]) ^ (~temporary & first[0]);
			result[0] += sigma1(temporary) + old_destination[0];
		} else if (operation == 0xce60'8400U) {
			const auto sigma0 = [](std::uint64_t value) {
				return std::rotr(value, 28) ^ std::rotr(value, 34) ^
					std::rotr(value, 39);
			};
			result[1] = (first[0] & second[1]) |
				((first[0] | second[1]) & second[0]);
			result[1] += sigma0(second[0]) + old_destination[1];
			result[0] = (result[1] & second[0]) |
				((result[1] | second[0]) & second[1]);
			result[0] += sigma0(result[1]) + old_destination[0];
		} else {
			for (std::size_t lane = 0; lane < 2; ++lane) {
				const auto value = first[lane];
				const auto sigma1 = std::rotr(value, 19) ^
					std::rotr(value, 61) ^ (value >> 6);
				result[lane] = old_destination[lane] + sigma1 + second[lane];
			}
		}
		store_doublewords(destination, result);
	} else if ((instruction & 0xffff'fc00U) == 0xcec0'8000U) {
		const auto old_destination = load_doublewords(destination);
		const auto source = load_doublewords(cpu.vectors[first_index]);
		const auto sigma0 = [](std::uint64_t value) {
			return std::rotr(value, 1) ^ std::rotr(value, 8) ^ (value >> 7);
		};
		store_doublewords(destination, {old_destination[0] +
			sigma0(old_destination[1]), old_destination[1] + sigma0(source[0])});
	} else if ((instruction & 0xffe0'fc00U) == 0xce60'c000U ||
		   (instruction & 0xffe0'fc00U) == 0xce60'c400U ||
		   (instruction & 0xffe0'fc00U) == 0xce60'c800U) {
		const auto old_destination = load_words(destination);
		const auto first = load_words(cpu.vectors[first_index]);
		const auto second = load_words(cpu.vectors[second_index]);
		std::array<std::uint32_t, 4> result{};
		const auto operation = instruction & 0xffe0'fc00U;
		if (operation == 0xce60'c000U) {
			for (std::size_t lane = 0; lane < 3; ++lane) {
				const auto value = (old_destination[lane] ^ first[lane]) ^
					std::rotl(second[lane + 1], 15);
				result[lane] = value ^ std::rotl(value, 15) ^
					std::rotl(value, 23);
			}
			const auto value = (old_destination[3] ^ first[3]) ^
				std::rotl(result[0], 15);
			result[3] = value ^ std::rotl(value, 15) ^ std::rotl(value, 23);
		} else if (operation == 0xce60'c400U) {
			std::array<std::uint32_t, 4> temporary{};
			for (std::size_t lane = 0; lane < 4; ++lane) {
				temporary[lane] = first[lane] ^ std::rotl(second[lane], 7);
				result[lane] = old_destination[lane] ^ temporary[lane];
			}
			auto correction = std::rotl(temporary[0], 15);
			correction ^= std::rotl(correction, 15) ^ std::rotl(correction, 23);
			result[3] ^= correction;
		} else {
			result = first;
			for (std::size_t round = 0; round < 4; ++round) {
				auto value = result[3] ^ result[2] ^ result[1] ^ second[round];
				value = sm4_substitute(value);
				value ^= std::rotl(value, 13) ^ std::rotl(value, 23) ^ result[0];
				result = {result[1], result[2], result[3], value};
			}
		}
		store_words(destination, result);
	} else if ((instruction & 0xffe0'8000U) == 0xce40'0000U) {
		const unsigned third_index = (instruction >> 10) & 31U;
		const auto first = load_words(cpu.vectors[first_index]);
		const auto second = load_words(cpu.vectors[second_index]);
		const auto third = load_words(cpu.vectors[third_index]);
		store_words(destination, {0, 0, 0, std::rotl(
			std::rotl(first[3], 12) + second[3] + third[3], 7)});
	} else if ((instruction & 0xffe0'cc00U) == 0xce40'8000U ||
		   (instruction & 0xffe0'cc00U) == 0xce40'8400U ||
		   (instruction & 0xffe0'cc00U) == 0xce40'8800U ||
		   (instruction & 0xffe0'cc00U) == 0xce40'8c00U) {
		const auto old_destination = load_words(destination);
		const auto first = load_words(cpu.vectors[first_index]);
		const auto second = load_words(cpu.vectors[second_index]);
		const unsigned lane = (instruction >> 12) & 3U;
		const unsigned operation = (instruction >> 10) & 3U;
		std::uint32_t value;
		if (operation == 0U)
			value = old_destination[1] ^ old_destination[3] ^ old_destination[2];
		else if (operation == 1U)
			value = (old_destination[3] & old_destination[1]) |
				(old_destination[3] & old_destination[2]) |
				(old_destination[1] & old_destination[2]);
		else if (operation == 2U)
			value = old_destination[1] ^ old_destination[3] ^ old_destination[2];
		else
			value = (old_destination[3] & old_destination[2]) |
				(~old_destination[3] & old_destination[1]);
		if (operation < 2U)
			value += old_destination[0] + (first[3] ^
				std::rotl(old_destination[3], 12)) + second[lane];
		else
			value += old_destination[0] + first[3] + second[lane];
		if (operation >= 2U)
			value ^= std::rotl(value, 9) ^ std::rotl(value, 17);
		store_words(destination, {old_destination[1],
			std::rotl(old_destination[2], operation < 2U ? 9 : 19),
			old_destination[3], value});
	} else if ((instruction & 0xffff'fc00U) == 0xcec0'8400U) {
		auto result = load_words(destination);
		const auto round_keys = load_words(cpu.vectors[first_index]);
		for (std::size_t round = 0; round < 4; ++round) {
			auto value = result[3] ^ result[2] ^ result[1] ^ round_keys[round];
			value = sm4_substitute(value);
			value ^= std::rotl(value, 2) ^ std::rotl(value, 10) ^
				std::rotl(value, 18) ^ std::rotl(value, 24) ^ result[0];
			result = {result[1], result[2], result[3], value};
		}
		store_words(destination, result);
	} else if ((instruction & 0xffe0'fc00U) == 0xce60'cc00U ||
		   (instruction & 0xffe0'8000U) == 0xce60'0000U ||
		   (instruction & 0xffff'fc00U) == 0xcec0'8800U ||
		   (instruction & 0xffff'fc00U) == 0xcec0'8c00U) {
		return {StopReason::illegal_instruction, address, instruction, 0,
			false};
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
