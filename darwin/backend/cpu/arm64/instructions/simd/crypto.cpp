// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../dispatcher.hpp"

#include "../../detail/instruction_helpers.hpp"
#include "runtime/host_features.hpp"

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

struct CarrylessProduct final {
	std::uint64_t low{};
	std::uint64_t high{};
};

/* Keep the interpreter's size=11 path independent of optional host
 * instructions. In particular, do not let this body inherit simd_crypto's
 * target attributes: the interpreter must remain safe on hosts without the
 * optional polynomial-multiply extension.
 */
[[gnu::noinline]]
[[nodiscard]] CarrylessProduct carryless_multiply_64(
	std::uint64_t left, std::uint64_t right) noexcept
{
	CarrylessProduct result{};
	for (unsigned bit = 0; bit < 64; ++bit) {
		if ((right & (std::uint64_t{1} << bit)) == 0)
			continue;
		result.low ^= left << bit;
		if (bit != 0)
			result.high ^= left >> (64U - bit);
	}
	return result;
}

[[nodiscard]] constexpr std::uint8_t multiply_finite_byte(
	std::uint8_t left, std::uint8_t right) noexcept
{
	std::uint8_t result = 0;
	for (unsigned bit = 0; bit < 8U; ++bit) {
		if ((right & 1U) != 0)
			result ^= left;
		const bool high = (left & 0x80U) != 0;
		left = static_cast<std::uint8_t>(left << 1U);
		if (high)
			left ^= 0x1bU;
		right = static_cast<std::uint8_t>(right >> 1U);
	}
	return result;
}

[[nodiscard]] constexpr std::uint8_t invert_finite_byte(
	std::uint8_t value) noexcept
{
	if (value == 0)
		return 0;
	std::uint8_t result = 1;
	std::uint8_t power = value;
	unsigned exponent = 254U;
	while (exponent != 0) {
		if ((exponent & 1U) != 0)
			result = multiply_finite_byte(result, power);
		power = multiply_finite_byte(power, power);
		exponent >>= 1U;
	}
	return result;
}

[[nodiscard]] constexpr std::uint8_t rotate_byte(
	std::uint8_t value, unsigned amount) noexcept
{
	return static_cast<std::uint8_t>(
		(value << amount) | (value >> (8U - amount)));
}

[[nodiscard]] constexpr std::uint8_t substitute_byte(
	std::uint8_t value, bool inverse) noexcept
{
	if (inverse) {
		const auto affine = static_cast<std::uint8_t>(
			rotate_byte(value, 1U) ^ rotate_byte(value, 3U) ^
			rotate_byte(value, 6U) ^ 0x05U);
		return invert_finite_byte(affine);
	}
	const auto reciprocal = invert_finite_byte(value);
	return static_cast<std::uint8_t>(
		reciprocal ^ rotate_byte(reciprocal, 1U) ^
		rotate_byte(reciprocal, 2U) ^
		rotate_byte(reciprocal, 3U) ^
		rotate_byte(reciprocal, 4U) ^ 0x63U);
}

template<bool inverse>
[[nodiscard]] consteval std::array<std::uint8_t, 256>
make_substitution_table()
{
	std::array<std::uint8_t, 256> result{};
	for (unsigned value = 0; value < result.size(); ++value)
		result[value] = substitute_byte(
			static_cast<std::uint8_t>(value), inverse);
	return result;
}

inline constexpr auto forward_substitution =
	make_substitution_table<false>();
inline constexpr auto inverse_substitution =
	make_substitution_table<true>();

template<bool inverse>
[[nodiscard]] std::array<std::byte, 16> shift_and_substitute(
	const std::array<std::byte, 16> &input)
{
	const auto &table =
		inverse ? inverse_substitution : forward_substitution;
	std::array<std::byte, 16> result{};
	for (unsigned column = 0; column < 4U; ++column)
		for (unsigned row = 0; row < 4U; ++row) {
			const auto source_column = inverse ?
				(column + 4U - row) & 3U :
				(column + row) & 3U;
			result[column * 4U + row] =
				static_cast<std::byte>(table[
					std::to_integer<std::uint8_t>(
						input[source_column * 4U + row])]);
		}
	return result;
}

template<bool inverse>
[[nodiscard]] std::array<std::byte, 16> mix_columns(
	const std::array<std::byte, 16> &input)
{
	std::array<std::byte, 16> result{};
	const std::array<std::uint8_t, 4> first_row =
		inverse ?
			std::array<std::uint8_t, 4>{14, 11, 13, 9} :
			std::array<std::uint8_t, 4>{2, 3, 1, 1};
	for (unsigned column = 0; column < 4U; ++column)
		for (unsigned row = 0; row < 4U; ++row) {
			std::uint8_t value = 0;
			for (unsigned source_row = 0; source_row < 4U;
			     ++source_row)
				value ^= multiply_finite_byte(
					std::to_integer<std::uint8_t>(
						input[column * 4U + source_row]),
					first_row[
						(source_row + 4U - row) & 3U]);
			result[column * 4U + row] =
				static_cast<std::byte>(value);
		}
	return result;
}

template<unsigned operation>
void execute_aes_software(VectorRegister &destination,
	const VectorRegister &source)
{
	static_assert(operation >= 4U && operation <= 7U);
	const auto old_destination = destination.bytes;
	const auto input = source.bytes;
	if constexpr (operation <= 5U) {
		std::array<std::byte, 16> combined{};
		for (std::size_t byte = 0; byte < combined.size(); ++byte)
			combined[byte] = old_destination[byte] ^ input[byte];
		destination.bytes =
			shift_and_substitute<operation == 5U>(combined);
	} else {
		destination.bytes = mix_columns<operation == 7U>(input);
	}
}

void execute_aes_software(VectorRegister &destination,
	const VectorRegister &source, unsigned operation)
{
	switch (operation) {
	case 4: execute_aes_software<4>(destination, source); break;
	case 5: execute_aes_software<5>(destination, source); break;
	case 6: execute_aes_software<6>(destination, source); break;
	case 7: execute_aes_software<7>(destination, source); break;
	default: __builtin_unreachable();
	}
}

/* Optional host instructions stay in this narrow leaf. The main dispatcher
 * and every portable helper are compiled for the baseline target.
 */
[[gnu::noinline, gnu::target("aes,neon")]]
void execute_aes_native(VectorRegister &destination,
	const VectorRegister &source, unsigned operation)
{
	const auto input = load_vector<uint8x16_t>(source);
	const auto old_destination = load_vector<uint8x16_t>(destination);
	uint8x16_t result;
	switch (operation) {
	case 4: result = vaeseq_u8(old_destination, input); break;
	case 5: result = vaesdq_u8(old_destination, input); break;
	case 6: result = vaesmcq_u8(input); break;
	case 7: result = vaesimcq_u8(input); break;
	default: __builtin_unreachable();
	}
	store_vector(destination, result);
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

template<unsigned operation>
void execute_aes_software_direct(
	CpuState *cpu, std::uintptr_t destination_offset,
	std::uintptr_t source_offset) noexcept
{
	auto *bytes = reinterpret_cast<std::byte *>(cpu);
	auto *destination =
		reinterpret_cast<VectorRegister *>(bytes + destination_offset);
	const auto *source =
		reinterpret_cast<const VectorRegister *>(bytes + source_offset);
	execute_aes_software<operation>(*destination, *source);
}

extern "C" void uml_arm64_simd_aese_software(
	CpuState *cpu, std::uintptr_t destination_offset,
	std::uintptr_t source_offset) noexcept
{
	execute_aes_software_direct<4>(
		cpu, destination_offset, source_offset);
}

extern "C" void uml_arm64_simd_aesd_software(
	CpuState *cpu, std::uintptr_t destination_offset,
	std::uintptr_t source_offset) noexcept
{
	execute_aes_software_direct<5>(
		cpu, destination_offset, source_offset);
}

extern "C" void uml_arm64_simd_aesmc_software(
	CpuState *cpu, std::uintptr_t destination_offset,
	std::uintptr_t source_offset) noexcept
{
	execute_aes_software_direct<6>(
		cpu, destination_offset, source_offset);
}

extern "C" void uml_arm64_simd_aesimc_software(
	CpuState *cpu, std::uintptr_t destination_offset,
	std::uintptr_t source_offset) noexcept
{
	execute_aes_software_direct<7>(
		cpu, destination_offset, source_offset);
}

extern "C" [[gnu::noinline]]
void uml_arm64_simd_pmull_software(
	CpuState *cpu, std::uintptr_t operand, unsigned high) noexcept
{
	const auto destination_offset = operand & 0xffffU;
	const auto left_offset = (operand >> 16) & 0xffffU;
	const auto right_offset = (operand >> 32) & 0xffffU;
	const auto lane_offset = high != 0 ? std::size_t{8} : std::size_t{};
	const auto *bytes = reinterpret_cast<const std::byte *>(cpu);
	const auto lhs = detail::decode_little_endian(
		std::span{bytes + left_offset + lane_offset, std::size_t{8}});
	const auto rhs = detail::decode_little_endian(
		std::span{bytes + right_offset + lane_offset, std::size_t{8}});
	const auto product = carryless_multiply_64(lhs, rhs);
	std::array<std::byte, 16> result{};
	detail::encode_little_endian(product.low,
		std::span{result}.first<8>());
	detail::encode_little_endian(product.high,
		std::span{result}.subspan<8, 8>());
	std::memcpy(reinterpret_cast<std::byte *>(cpu) + destination_offset,
		result.data(), result.size());
}

namespace {

[[nodiscard]] VectorRegister &vector_at(CpuState *cpu,
                                        std::uintptr_t offset) noexcept {
  return *reinterpret_cast<VectorRegister *>(
      reinterpret_cast<std::byte *>(cpu) + offset);
}

template <unsigned operation>
void execute_sha12_three_software(CpuState *cpu,
                                  std::uintptr_t destination_offset,
                                  std::uintptr_t first_offset,
                                  std::uintptr_t second_offset) noexcept {
  auto &destination = vector_at(cpu, destination_offset);
  const auto first = load_words(vector_at(cpu, first_offset));
  const auto second = load_words(vector_at(cpu, second_offset));
  const auto old_destination = load_words(destination);
  std::array<std::uint32_t, 4> result{};
  if constexpr (operation == 0U)
    result = sha1_hash(old_destination, first[0], second, 0);
  else if constexpr (operation == 1U)
    result = sha1_hash(old_destination, first[0], second, 1);
  else if constexpr (operation == 2U)
    result = sha1_hash(old_destination, first[0], second, 2);
  else if constexpr (operation == 3U)
    result = {
        second[0] ^ old_destination[0] ^ old_destination[2],
        second[1] ^ old_destination[1] ^ old_destination[3],
        second[2] ^ old_destination[2] ^ first[0],
        second[3] ^ old_destination[3] ^ first[1],
    };
  else if constexpr (operation == 4U)
    result = sha256_hash(old_destination, first, second, true);
  else if constexpr (operation == 5U)
    result = sha256_hash(first, old_destination, second, false);
  else {
    const std::array temporary{
        first[1],
        first[2],
        first[3],
        second[0],
    };
    for (std::size_t lane = 0; lane < 2; ++lane) {
      const auto value = second[lane + 2];
      result[lane] =
          (std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10)) +
          old_destination[lane] + temporary[lane];
    }
    for (std::size_t lane = 2; lane < 4; ++lane) {
      const auto value = result[lane - 2];
      result[lane] =
          (std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10)) +
          old_destination[lane] + temporary[lane];
    }
  }
  store_words(destination, result);
}

template <unsigned operation>
void execute_sha12_two_software(CpuState *cpu,
                                std::uintptr_t destination_offset,
                                std::uintptr_t source_offset) noexcept {
  auto &destination = vector_at(cpu, destination_offset);
  const auto source = load_words(vector_at(cpu, source_offset));
  if constexpr (operation == 0U) {
    store_word(destination, std::rotl(source[0], 30));
  } else if constexpr (operation == 1U) {
    const auto old_destination = load_words(destination);
    const std::array temporary{
        old_destination[0] ^ source[1],
        old_destination[1] ^ source[2],
        old_destination[2] ^ source[3],
        old_destination[3],
    };
    store_words(destination,
                {
                    std::rotl(temporary[0], 1),
                    std::rotl(temporary[1], 1),
                    std::rotl(temporary[2], 1),
                    std::rotl(temporary[3], 1) ^ std::rotl(temporary[0], 2),
                });
  } else {
    const auto old_destination = load_words(destination);
    const std::array temporary{
        old_destination[1],
        old_destination[2],
        old_destination[3],
        source[0],
    };
    std::array<std::uint32_t, 4> result{};
    for (std::size_t lane = 0; lane < result.size(); ++lane) {
      const auto value = temporary[lane];
      result[lane] =
          old_destination[lane] +
          (std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3));
    }
    store_words(destination, result);
  }
}

template <unsigned operation>
void execute_sha512_three_software(CpuState *cpu,
                                   std::uintptr_t destination_offset,
                                   std::uintptr_t first_offset,
                                   std::uintptr_t second_offset) noexcept {
  auto &destination = vector_at(cpu, destination_offset);
  const auto first = load_doublewords(vector_at(cpu, first_offset));
  const auto second = load_doublewords(vector_at(cpu, second_offset));
  const auto old_destination = load_doublewords(destination);
  std::array<std::uint64_t, 2> result{};
  if constexpr (operation == 0U) {
    const auto sigma1 = [](std::uint64_t value) {
      return std::rotr(value, 14) ^ std::rotr(value, 18) ^ std::rotr(value, 41);
    };
    result[1] = (second[1] & first[0]) ^ (~second[1] & first[1]);
    result[1] += sigma1(second[1]) + old_destination[1];
    const auto temporary = result[1] + second[0];
    result[0] = (temporary & second[1]) ^ (~temporary & first[0]);
    result[0] += sigma1(temporary) + old_destination[0];
  } else if constexpr (operation == 1U) {
    const auto sigma0 = [](std::uint64_t value) {
      return std::rotr(value, 28) ^ std::rotr(value, 34) ^ std::rotr(value, 39);
    };
    result[1] = (first[0] & second[1]) | ((first[0] | second[1]) & second[0]);
    result[1] += sigma0(second[0]) + old_destination[1];
    result[0] = (result[1] & second[0]) | ((result[1] | second[0]) & second[1]);
    result[0] += sigma0(result[1]) + old_destination[0];
  } else {
    for (std::size_t lane = 0; lane < result.size(); ++lane) {
      const auto value = first[lane];
      const auto sigma1 =
          std::rotr(value, 19) ^ std::rotr(value, 61) ^ (value >> 6);
      result[lane] = old_destination[lane] + sigma1 + second[lane];
    }
  }
  store_doublewords(destination, result);
}

void execute_sha512_two_software(CpuState *cpu,
                                 std::uintptr_t destination_offset,
                                 std::uintptr_t source_offset) noexcept {
  auto &destination = vector_at(cpu, destination_offset);
  const auto old_destination = load_doublewords(destination);
  const auto source = load_doublewords(vector_at(cpu, source_offset));
  const auto sigma0 = [](std::uint64_t value) {
    return std::rotr(value, 1) ^ std::rotr(value, 8) ^ (value >> 7);
  };
  store_doublewords(destination,
                    {
                        old_destination[0] + sigma0(old_destination[1]),
                        old_destination[1] + sigma0(source[0]),
                    });
}

} // namespace

#define UML_DEFINE_SHA12_THREE_SOFTWARE(name, operation)                       \
  extern "C" void uml_arm64_simd_##name##_software(                            \
      CpuState *cpu, std::uintptr_t destination_offset,                        \
      std::uintptr_t first_offset, std::uintptr_t second_offset) noexcept {    \
    execute_sha12_three_software<operation>(cpu, destination_offset,           \
                                            first_offset, second_offset);      \
  }
UML_DEFINE_SHA12_THREE_SOFTWARE(sha1c, 0)
UML_DEFINE_SHA12_THREE_SOFTWARE(sha1p, 1)
UML_DEFINE_SHA12_THREE_SOFTWARE(sha1m, 2)
UML_DEFINE_SHA12_THREE_SOFTWARE(sha1su0, 3)
UML_DEFINE_SHA12_THREE_SOFTWARE(sha256h, 4)
UML_DEFINE_SHA12_THREE_SOFTWARE(sha256h2, 5)
UML_DEFINE_SHA12_THREE_SOFTWARE(sha256su1, 6)
#undef UML_DEFINE_SHA12_THREE_SOFTWARE

#define UML_DEFINE_SHA12_TWO_SOFTWARE(name, operation)                         \
  extern "C" void uml_arm64_simd_##name##_software(                            \
      CpuState *cpu, std::uintptr_t destination_offset,                        \
      std::uintptr_t source_offset) noexcept {                                 \
    execute_sha12_two_software<operation>(cpu, destination_offset,             \
                                          source_offset);                      \
  }
UML_DEFINE_SHA12_TWO_SOFTWARE(sha1h, 0)
UML_DEFINE_SHA12_TWO_SOFTWARE(sha1su1, 1)
UML_DEFINE_SHA12_TWO_SOFTWARE(sha256su0, 2)
#undef UML_DEFINE_SHA12_TWO_SOFTWARE

#define UML_DEFINE_SHA512_THREE_SOFTWARE(name, operation)                      \
  extern "C" void uml_arm64_simd_##name##_software(                            \
      CpuState *cpu, std::uintptr_t destination_offset,                        \
      std::uintptr_t first_offset, std::uintptr_t second_offset) noexcept {    \
    execute_sha512_three_software<operation>(cpu, destination_offset,          \
                                             first_offset, second_offset);     \
  }
UML_DEFINE_SHA512_THREE_SOFTWARE(sha512h, 0)
UML_DEFINE_SHA512_THREE_SOFTWARE(sha512h2, 1)
UML_DEFINE_SHA512_THREE_SOFTWARE(sha512su1, 2)
#undef UML_DEFINE_SHA512_THREE_SOFTWARE

extern "C" void
uml_arm64_simd_sha512su0_software(CpuState *cpu,
                                  std::uintptr_t destination_offset,
                                  std::uintptr_t source_offset) noexcept {
  execute_sha512_two_software(cpu, destination_offset, source_offset);
}

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
		if (host_arm_features().has(ArmFeature::advanced_simd) &&
		    host_arm_features().has(ArmFeature::aes))
			execute_aes_native(
				destination, cpu.vectors[first_index], operation);
		else
			execute_aes_software(
				destination, cpu.vectors[first_index], operation);
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
			const auto lhs = detail::decode_little_endian(
				std::span{left}.subspan(source_offset, 8));
			const auto rhs = detail::decode_little_endian(
				std::span{right}.subspan(source_offset, 8));
			const auto product = carryless_multiply_64(lhs, rhs);
			store_doublewords(destination, {product.low, product.high});
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
		const auto first = cpu.vectors[first_index].bytes;
		const auto second = cpu.vectors[second_index].bytes;
		const auto third = cpu.vectors[third_index].bytes;
		std::array<std::byte, 16> result{};
		const bool bit_clear = (instruction & 0x0020'0000U) != 0;
		for (std::size_t byte = 0; byte < result.size(); ++byte)
			result[byte] = bit_clear ?
				first[byte] ^ (second[byte] & ~third[byte]) :
				first[byte] ^ second[byte] ^ third[byte];
		destination.bytes = result;
	} else if ((instruction & 0xffe0'fc00U) == 0xce60'8c00U) {
		const auto first = cpu.vectors[first_index].bytes;
		const auto second = cpu.vectors[second_index].bytes;
		std::array<std::byte, 16> result{};
		for (std::size_t lane = 0; lane < 2; ++lane) {
			const auto left = detail::decode_little_endian(
				std::span{first}.subspan(lane * 8, 8));
			const auto right = detail::decode_little_endian(
				std::span{second}.subspan(lane * 8, 8));
			detail::encode_little_endian(
				left ^ std::rotl(right, 1),
				std::span{result}.subspan(lane * 8, 8));
		}
		destination.bytes = result;
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
