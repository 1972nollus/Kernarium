// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The SVE2 cryptographic forms (AESE, AESD, AESMC, AESIMC and RAX1) and the
 * integer matrix multiply-accumulates of FEAT_I8MM (SMMLA, UMMLA and
 * USMMLA) at PSTATE.SM = 0 (sve_semantics.hpp). Each works on the 128-bit
 * segments of its vectors, of which VL 16 has one, in software, so that no
 * host feature beyond the base architecture is needed.
 */
#include "sve_semantics_internal.hpp"

namespace uml::cpu::arm64::detail::sve_semantics {

namespace {

/* Multiplication in GF(2^8) modulo the AES polynomial x^8+x^4+x^3+x+1. */
[[nodiscard]] constexpr std::uint8_t multiply(
	std::uint8_t a, std::uint8_t b) noexcept
{
	unsigned product = 0;
	unsigned value = a;
	for (unsigned bit = 0; bit < 8U; ++bit) {
		if ((b >> bit & 1U) != 0U)
			product ^= value;
		value <<= 1U;
		if ((value & 0x100U) != 0U)
			value ^= 0x11bU;
	}
	return static_cast<std::uint8_t>(product);
}

[[nodiscard]] constexpr std::uint8_t rotate_byte(
	std::uint8_t value, unsigned amount) noexcept
{
	return static_cast<std::uint8_t>(
		value << amount | value >> (8U - amount));
}

/* The multiplicative inverse, value^254, and 0 for 0. */
[[nodiscard]] constexpr std::uint8_t inverse_of(std::uint8_t value) noexcept
{
	std::uint8_t result = 1;
	std::uint8_t power = value;
	for (unsigned exponent = 254U; exponent != 0U; exponent >>= 1U) {
		if ((exponent & 1U) != 0U)
			result = multiply(result, power);
		power = multiply(power, power);
	}
	return result;
}

/* FIPS-197 S-box: the multiplicative inverse followed by the affine map. */
[[nodiscard]] consteval std::array<std::uint8_t, 256> forward_box()
{
	std::array<std::uint8_t, 256> box{};
	for (unsigned value = 0; value < 256U; ++value) {
		const std::uint8_t inverse =
			inverse_of(static_cast<std::uint8_t>(value));
		box[value] = static_cast<std::uint8_t>(inverse ^
			rotate_byte(inverse, 1) ^ rotate_byte(inverse, 2) ^
			rotate_byte(inverse, 3) ^ rotate_byte(inverse, 4) ^
			0x63U);
	}
	return box;
}

[[nodiscard]] consteval std::array<std::uint8_t, 256> inverse_box()
{
	const auto forward = forward_box();
	std::array<std::uint8_t, 256> box{};
	for (unsigned value = 0; value < 256U; ++value)
		box[forward[value]] = static_cast<std::uint8_t>(value);
	return box;
}

constexpr auto sub_box = forward_box();
constexpr auto inverse_sub_box = inverse_box();
static_assert(sub_box[0x00] == 0x63U && sub_box[0x53] == 0xedU);
static_assert(inverse_sub_box[0x63] == 0x00U);

using Block = std::array<std::uint8_t, 16>;

/* The state's byte r + 4c is row r of column c. */
[[nodiscard]] Block shift_rows(const Block &in, bool inverse) noexcept
{
	Block out{};
	for (unsigned c = 0; c < 4U; ++c)
		for (unsigned r = 0; r < 4U; ++r) {
			const unsigned shifted = r + 4U * ((c + r) % 4U);
			if (inverse)
				out[shifted] = in[r + 4U * c];
			else
				out[r + 4U * c] = in[shifted];
		}
	return out;
}

[[nodiscard]] Block mix_columns(const Block &in, bool inverse) noexcept
{
	constexpr std::array<std::uint8_t, 4> forward{2, 3, 1, 1};
	constexpr std::array<std::uint8_t, 4> backward{14, 11, 13, 9};
	const auto &coefficients = inverse ? backward : forward;
	Block out{};
	for (unsigned c = 0; c < 4U; ++c)
		for (unsigned r = 0; r < 4U; ++r) {
			std::uint8_t sum = 0;
			for (unsigned k = 0; k < 4U; ++k)
				sum ^= multiply(coefficients[(k + 4U - r) % 4U],
					in[k + 4U * c]);
			out[r + 4U * c] = sum;
		}
	return out;
}

[[nodiscard]] Block block_of(const Vector &vector) noexcept
{
	Block block;
	std::memcpy(block.data(), vector.bytes.data(), block.size());
	return block;
}

[[nodiscard]] Vector vector_of(const Block &block) noexcept
{
	Vector vector;
	std::memcpy(vector.bytes.data(), block.data(), block.size());
	return vector;
}

/* AESE and AESD: AddRoundKey, then (Inv)ShiftRows and (Inv)SubBytes. */
[[nodiscard]] SveResult aes_round(Context &context) noexcept
{
	const bool decrypt = context.bit(10);
	const unsigned dn = context.field(0, 5);
	Block state = block_of(context.z(dn));
	const Block key = block_of(context.z(context.field(5, 5)));
	for (unsigned byte = 0; byte < state.size(); ++byte)
		state[byte] ^= key[byte];
	state = shift_rows(state, decrypt);
	for (auto &byte : state)
		byte = decrypt ? inverse_sub_box[byte] : sub_box[byte];
	context.set_z(dn, vector_of(state));
	return retired();
}

/* AESMC and AESIMC. */
[[nodiscard]] SveResult aes_mix(Context &context) noexcept
{
	const unsigned dn = context.field(0, 5);
	context.set_z(dn,
		vector_of(
			mix_columns(block_of(context.z(dn)), context.bit(10))));
	return retired();
}

/* RAX1: Zd.D = Zn.D EOR (Zm.D ROL 1). */
[[nodiscard]] SveResult rotate_xor(Context &context) noexcept
{
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	Vector result;
	for (unsigned element = 0; element < elements_of<std::uint64_t>;
		++element)
		result.set<std::uint64_t>(element,
			n.get<std::uint64_t>(element) ^
				std::rotl(m.get<std::uint64_t>(element), 1));
	context.set_z(context.field(0, 5), result);
	return retired();
}

/*
 * SMMLA, UMMLA and USMMLA: per segment, the 2x2 matrix of 32-bit integers
 * in Zda plus the product of Zn's 2x8 matrix of bytes and the transpose of
 * Zm's, wrapping. Bits 23:22 give the signedness: 00 both signed, 11 both
 * unsigned, 10 Zn unsigned and Zm signed.
 */
[[nodiscard]] SveResult matrix_multiply_add(Context &context) noexcept
{
	const unsigned size = context.field(22, 2);
	const bool first_unsigned = size != 0U;
	const bool second_unsigned = size == 3U;
	const Vector n = context.z(context.field(5, 5));
	const Vector m = context.z(context.field(16, 5));
	const unsigned da = context.field(0, 5);
	Vector result = context.z(da);
	for (unsigned i = 0; i < 2U; ++i)
		for (unsigned j = 0; j < 2U; ++j) {
			std::uint32_t sum =
				result.get<std::uint32_t>(2U * i + j);
			for (unsigned k = 0; k < 8U; ++k) {
				const std::uint8_t a = n.bytes[8U * i + k];
				const std::uint8_t b = m.bytes[8U * j + k];
				const std::int32_t left = first_unsigned ?
					a :
					static_cast<std::int8_t>(a);
				const std::int32_t right = second_unsigned ?
					b :
					static_cast<std::int8_t>(b);
				sum += static_cast<std::uint32_t>(left * right);
			}
			result.set<std::uint32_t>(2U * i + j, sum);
		}
	context.set_z(da, result);
	return retired();
}

} // namespace

Handler crypto_handler(std::uint16_t id) noexcept
{
	switch (id) {
	case form::aese_z_zz:
	case form::aesd_z_zz:
		return aes_round;
	case form::aesmc_z_z:
	case form::aesimc_z_z:
		return aes_mix;
	case form::rax1_z_zz:
		return rotate_xor;
	case form::smmla_z_zzz:
	case form::ummla_z_zzz:
	case form::usmmla_z_zzz:
		return matrix_multiply_add;
	default:
		return nullptr;
	}
}

} // namespace uml::cpu::arm64::detail::sve_semantics
