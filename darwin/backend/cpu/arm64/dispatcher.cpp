// SPDX-License-Identifier: GPL-2.0-or-later
// Static-gadget dispatch design derived from iSH's arm64 asbestos backend.
#include "dispatcher.hpp"

#include "detail/instruction_helpers.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::backend::arm64 {

RunResult Dispatcher::run(CpuState &cpu, GuestMemory &memory,
			  std::size_t budget) const
{
	for (std::size_t completed = 0; completed < budget; ++completed) {
		auto result = step(cpu, memory);
		if (result.reason != StopReason::continued) {
			result.instructions += completed;
			return result;
		}
	}
	return {StopReason::budget_exhausted, cpu.pc, 0, budget, false};
}

RunResult Dispatcher::step(CpuState &cpu, GuestMemory &memory) const
{
	std::array<std::byte, sizeof(std::uint32_t)> encoded{};
	if (!memory.fetch(cpu.pc, encoded))
		return {StopReason::memory_fault, cpu.pc, 0, 0, false};
	const auto instruction = static_cast<std::uint32_t>(
		detail::decode_little_endian(encoded));
	return execute(cpu, memory, instruction);
}

RunResult Dispatcher::execute(CpuState &cpu, GuestMemory &memory,
			      std::uint32_t instruction) const
{
	/* UDF #imm16 is the permanently undefined A64 instruction class.  It is
	 * architecturally distinct from an otherwise valid instruction which this
	 * emulator has not implemented yet: userspace must receive SIGILL for UDF,
	 * while missing emulation remains a fatal diagnostic.
	 */
	if ((instruction & 0xffff'0000U) == 0)
		return {StopReason::illegal_instruction, cpu.pc, instruction, 0,
			false};

	static constexpr std::array decoders{
		DecodeEntry{0xffe0'001fU, 0xd400'0001U, &Dispatcher::exception},
		DecodeEntry{0xffe0'001fU, 0xd420'0000U, &Dispatcher::exception},
		/* Architectural HINTs, including NOP and BTI landing pads, have no
		 * guest-visible state in this userspace emulator.
		 */
		DecodeEntry{0xffff'f01fU, 0xd503'201fU, &Dispatcher::nop},
		DecodeEntry{0xffff'ffe0U, 0xd53b'd040U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd51b'd040U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd53b'00e0U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd53b'0020U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd53b'4400U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd51b'4400U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd53b'4420U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd51b'4420U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd53b'4200U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd51b'4200U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd53b'e000U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd53b'e040U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd50b'7b20U,
			&Dispatcher::cache_maintenance}, // dc cvau, Xt
		DecodeEntry{0xffff'ffe0U, 0xd50b'7520U,
			&Dispatcher::cache_maintenance}, // ic ivau, Xt
		DecodeEntry{0xffff'f0ffU, 0xd503'309fU, &Dispatcher::memory_barrier},
		DecodeEntry{0xffff'f0ffU, 0xd503'30bfU, &Dispatcher::memory_barrier},
		DecodeEntry{0xffff'f0ffU, 0xd503'30dfU, &Dispatcher::memory_barrier},
		DecodeEntry{0xffff'fc1fU, 0xd61f'0000U, &Dispatcher::branch_register},
		DecodeEntry{0xffff'fc1fU, 0xd63f'0000U, &Dispatcher::branch_register},
		DecodeEntry{0xffff'fc1fU, 0xd65f'0000U, &Dispatcher::branch_register},
		DecodeEntry{0xff00'0010U, 0x5400'0000U, &Dispatcher::branch_conditional},
		DecodeEntry{0x7e00'0000U, 0x3400'0000U, &Dispatcher::compare_branch},
		DecodeEntry{0x7e00'0000U, 0x3600'0000U, &Dispatcher::test_branch},
		DecodeEntry{0x7c00'0000U, 0x1400'0000U, &Dispatcher::branch_immediate},
		DecodeEntry{0x1f00'0000U, 0x1000'0000U, &Dispatcher::pc_relative},
		DecodeEntry{0x1f80'0000U, 0x1280'0000U, &Dispatcher::move_wide},
		DecodeEntry{0x1f00'0000U, 0x1100'0000U, &Dispatcher::add_sub_immediate},
		DecodeEntry{0x1f00'0000U, 0x0b00'0000U, &Dispatcher::add_sub_register},
		DecodeEntry{0x1fe0'fc00U, 0x1a00'0000U, &Dispatcher::add_sub_carry},
		DecodeEntry{0x1f80'0000U, 0x1200'0000U, &Dispatcher::logical_immediate},
		DecodeEntry{0x1f80'0000U, 0x1300'0000U, &Dispatcher::bitfield},
		DecodeEntry{0xffe0'0000U, 0x93c0'0000U, &Dispatcher::extract_register},
		DecodeEntry{0xffe0'0000U, 0x1380'0000U, &Dispatcher::extract_register},
		DecodeEntry{0x1f00'0000U, 0x0a00'0000U, &Dispatcher::logical_shifted},
		DecodeEntry{0x3fe0'0000U, 0x1a80'0000U, &Dispatcher::conditional_select},
		DecodeEntry{0x3fe0'0410U, 0x3a40'0000U,
			&Dispatcher::conditional_compare},
		DecodeEntry{0x1f00'0000U, 0x1b00'0000U, &Dispatcher::multiply_add},
		DecodeEntry{0x7fe0'fc00U, 0x1ac0'2000U,
			&Dispatcher::variable_shift},
		DecodeEntry{0x7fe0'fc00U, 0x1ac0'2400U,
			&Dispatcher::variable_shift},
		DecodeEntry{0x7fe0'fc00U, 0x1ac0'2800U,
			&Dispatcher::variable_shift},
		DecodeEntry{0x7fe0'fc00U, 0x1ac0'2c00U,
			&Dispatcher::variable_shift},
		DecodeEntry{0x7fe0'fc00U, 0x1ac0'0800U, &Dispatcher::divide},
		DecodeEntry{0x7fe0'fc00U, 0x1ac0'0c00U, &Dispatcher::divide},
		DecodeEntry{0x7fff'8000U, 0x5ac0'0000U,
			&Dispatcher::bit_reverse_count},
		DecodeEntry{0x7fe0'e000U, 0x1ac0'4000U, &Dispatcher::crc32},
		DecodeEntry{0xff20'fc00U, 0x1e20'2000U,
			&Dispatcher::floating_compare},
		DecodeEntry{0xff20'0c00U, 0x1e20'0400U,
			&Dispatcher::floating_conditional_compare},
		DecodeEntry{0xff20'0c00U, 0x1e20'0c00U,
			&Dispatcher::floating_conditional_select},
		DecodeEntry{0xff00'0000U, 0x1f00'0000U,
			&Dispatcher::floating_multiply_add},
		DecodeEntry{0xff20'1fe0U, 0x1e20'1000U,
			&Dispatcher::floating_move},
		DecodeEntry{0xffbf'fc00U, 0x1e20'4000U,
			&Dispatcher::floating_move},
		DecodeEntry{0xffff'fc00U, 0x9e66'0000U,
			&Dispatcher::simd_general_move},
		DecodeEntry{0xffff'fc00U, 0x9e67'0000U,
			&Dispatcher::simd_general_move},
		DecodeEntry{0xffff'fc00U, 0x1e26'0000U,
			&Dispatcher::simd_general_move},
		DecodeEntry{0xffff'fc00U, 0x1e27'0000U,
			&Dispatcher::simd_general_move},
		DecodeEntry{0xffff'fc00U, 0x9eae'0000U,
			&Dispatcher::simd_general_move},
		DecodeEntry{0xffff'fc00U, 0x9eaf'0000U,
			&Dispatcher::simd_general_move},
		DecodeEntry{0xbfe0'fc00U, 0x0e00'3c00U,
			&Dispatcher::simd_general_move},
		DecodeEntry{0xbfe0'fc00U, 0x0e00'2c00U,
			&Dispatcher::simd_general_move},
		DecodeEntry{0x7f20'0000U, 0x1e00'0000U,
			&Dispatcher::floating_integer_convert},
		DecodeEntry{0x7f20'fc00U, 0x1e20'0000U,
			&Dispatcher::floating_integer_convert},
		DecodeEntry{0xff20'7c00U, 0x1e20'4000U,
			&Dispatcher::floating_arithmetic},
		DecodeEntry{0xff20'0c00U, 0x1e20'0800U,
			&Dispatcher::floating_arithmetic},
		DecodeEntry{0xffe0'fc00U, 0x4e00'1c00U,
			&Dispatcher::simd_insert_general},
		DecodeEntry{0xffe0'8400U, 0x6e00'0400U,
			&Dispatcher::simd_insert_element},
		DecodeEntry{0xbfe0'fc00U, 0x0e00'0c00U,
			&Dispatcher::simd_duplicate_general},
		DecodeEntry{0xbf20'fc00U, 0x0e00'0400U,
			&Dispatcher::simd_duplicate_element},
		DecodeEntry{0xff20'fc00U, 0x5e00'0400U,
			&Dispatcher::simd_duplicate_element}, // scalar mov from vector lane
		/* Cryptographic extension families.  Keep these
		 * entries ahead of the broad Advanced SIMD classes below.
		 */
		DecodeEntry{0xffff'8c00U, 0x4e28'0800U,
			&Dispatcher::simd_crypto}, // aese/aesd/aesmc/aesimc
		DecodeEntry{0xbf20'fc00U, 0x0e20'e000U,
			&Dispatcher::simd_crypto}, // pmull/pmull2
		DecodeEntry{0xffe0'8c00U, 0x5e00'0000U,
			&Dispatcher::simd_crypto}, // SHA three-register forms
		DecodeEntry{0xffff'8c00U, 0x5e28'0800U,
			&Dispatcher::simd_crypto}, // SHA two-register forms
		DecodeEntry{0xffe0'fc00U, 0xce60'8000U,
			&Dispatcher::simd_crypto}, // sha512h
		DecodeEntry{0xffe0'fc00U, 0xce60'8400U,
			&Dispatcher::simd_crypto}, // sha512h2
		DecodeEntry{0xffff'fc00U, 0xcec0'8000U,
			&Dispatcher::simd_crypto}, // sha512su0
		DecodeEntry{0xffe0'fc00U, 0xce60'8800U,
			&Dispatcher::simd_crypto}, // sha512su1
		DecodeEntry{0xffe0'8000U, 0xce00'0000U,
			&Dispatcher::simd_crypto}, // eor3
		DecodeEntry{0xffe0'fc00U, 0xce60'8c00U,
			&Dispatcher::simd_crypto}, // rax1
		DecodeEntry{0xffe0'0000U, 0xce80'0000U,
			&Dispatcher::simd_crypto}, // xar
		DecodeEntry{0xffe0'8000U, 0xce20'0000U,
			&Dispatcher::simd_crypto}, // bcax
		DecodeEntry{0x9ff8'0c00U, 0x0f00'0400U,
			&Dispatcher::simd_modified_immediate},
		DecodeEntry{0x9f3f'fc00U, 0x0e20'8800U,
			&Dispatcher::simd_compare_zero},
		DecodeEntry{0x9f3f'fc00U, 0x0e20'9800U,
			&Dispatcher::simd_compare_zero},
		DecodeEntry{0x9f3f'fc00U, 0x0e20'a800U,
			&Dispatcher::simd_compare_zero},
		DecodeEntry{0x9f3f'fc00U, 0x1e20'8800U,
			&Dispatcher::simd_compare_zero},
		DecodeEntry{0x9f3f'fc00U, 0x1e20'9800U,
			&Dispatcher::simd_compare_zero},
		DecodeEntry{0x9f3f'fc00U, 0x1e20'a800U,
			&Dispatcher::simd_compare_zero},
		DecodeEntry{0x9f20'fc00U, 0x0e20'3400U,
			&Dispatcher::simd_compare_register},
		DecodeEntry{0x9f20'fc00U, 0x0e20'3c00U,
			&Dispatcher::simd_compare_register},
		DecodeEntry{0x9f20'fc00U, 0x0e20'8c00U,
			&Dispatcher::simd_compare_register},
		DecodeEntry{0x9f20'fc00U, 0x0e20'1c00U,
			&Dispatcher::simd_bitwise_register},
		DecodeEntry{0x9f20'fc00U, 0x0e20'6400U,
			&Dispatcher::simd_min_max},
		DecodeEntry{0x9f20'fc00U, 0x0e20'6c00U,
			&Dispatcher::simd_min_max},
		DecodeEntry{0x9f20'fc00U, 0x0e20'a400U,
			&Dispatcher::simd_min_max},
		DecodeEntry{0x9f20'fc00U, 0x0e20'ac00U,
			&Dispatcher::simd_min_max},
		DecodeEntry{0x9f20'fc00U, 0x0e20'5800U,
			&Dispatcher::simd_bit_unary},
		DecodeEntry{0x9f20'fc00U, 0x0e20'0800U,
			&Dispatcher::simd_bit_unary},
		DecodeEntry{0x9f20'fc00U, 0x0e20'1800U,
			&Dispatcher::simd_bit_unary},
		DecodeEntry{0x9f3f'fc00U, 0x0e31'b800U,
			&Dispatcher::simd_reduce_integer},
		DecodeEntry{0x9f3f'fc00U, 0x0e30'a800U,
			&Dispatcher::simd_reduce_integer},
		DecodeEntry{0x9f3f'fc00U, 0x0e31'a800U,
			&Dispatcher::simd_reduce_integer},
		DecodeEntry{0x9f3f'fc00U, 0x0e30'3800U,
			&Dispatcher::simd_reduce_integer},
		DecodeEntry{0xbfe0'8400U, 0x2e00'0000U,
			&Dispatcher::simd_extract},
		DecodeEntry{0xbf20'fc00U, 0x0e00'1800U,
			&Dispatcher::simd_permute}, // uzp1
		DecodeEntry{0xbf20'fc00U, 0x0e00'5800U,
			&Dispatcher::simd_permute}, // uzp2
		DecodeEntry{0xbf20'fc00U, 0x0e00'2800U,
			&Dispatcher::simd_permute}, // trn1
		DecodeEntry{0xbf20'fc00U, 0x0e00'6800U,
			&Dispatcher::simd_permute}, // trn2
		DecodeEntry{0xbf20'fc00U, 0x0e00'3800U,
			&Dispatcher::simd_permute}, // zip1
		DecodeEntry{0xbf20'fc00U, 0x0e00'7800U,
			&Dispatcher::simd_permute}, // zip2
		DecodeEntry{0x9f20'fc00U, 0x0e20'8400U,
			&Dispatcher::simd_arithmetic_register},
		DecodeEntry{0x9f20'fc00U, 0x1e20'8400U,
			&Dispatcher::simd_arithmetic_register}, // scalar add/sub Dd
		DecodeEntry{0xffff'fc00U, 0x5ef1'b800U,
			&Dispatcher::simd_arithmetic_register}, // scalar addp Dd, Vn.2D
		DecodeEntry{0xbf20'fc00U, 0x0e20'bc00U,
			&Dispatcher::simd_arithmetic_register},
		DecodeEntry{0xbf20'fc00U, 0x0e20'9c00U,
			&Dispatcher::simd_arithmetic_register},
		DecodeEntry{0x9f20'fc00U, 0x0e20'4000U,
			&Dispatcher::simd_arithmetic_narrow},
		DecodeEntry{0x9f20'fc00U, 0x0e20'6000U,
			&Dispatcher::simd_arithmetic_narrow},
		DecodeEntry{0x9f20'0c00U, 0x0e20'0000U,
			&Dispatcher::simd_arithmetic_wide},
		DecodeEntry{0xdf20'0c00U, 0x5e20'0000U,
			&Dispatcher::simd_arithmetic_wide},
		DecodeEntry{0xbf3f'fc00U, 0x0e21'2800U,
			&Dispatcher::simd_narrow_move},
		DecodeEntry{0xafbb'fc00U, 0x0e21'd800U,
			&Dispatcher::simd_integer_float_convert},
		DecodeEntry{0xafbb'fc00U, 0x2e21'd800U,
			&Dispatcher::simd_integer_float_convert},
		DecodeEntry{0xafbb'fc00U, 0x0ea1'b800U,
			&Dispatcher::simd_integer_float_convert},
		DecodeEntry{0xafbb'fc00U, 0x2ea1'b800U,
			&Dispatcher::simd_integer_float_convert},
		DecodeEntry{0xbf3f'fc00U, 0x0e20'f800U,
			&Dispatcher::simd_floating_unary}, // fabs
		DecodeEntry{0xbf3f'fc00U, 0x2e20'f800U,
			&Dispatcher::simd_floating_unary}, // fneg
		DecodeEntry{0xbf3f'fc00U, 0x2e21'f800U,
			&Dispatcher::simd_floating_unary}, // fsqrt
		DecodeEntry{0x9f30'0c00U, 0x0e20'0800U,
			&Dispatcher::simd_two_register_misc},
		DecodeEntry{0xdf30'0c00U, 0x5e20'0800U,
			&Dispatcher::simd_two_register_misc},
		DecodeEntry{0x9f30'0c00U, 0x0e30'0800U,
			&Dispatcher::simd_across_lanes},
		DecodeEntry{0xdf30'0c00U, 0x5e30'0800U,
			&Dispatcher::simd_scalar_pairwise},
		DecodeEntry{0xbfe0'8c00U, 0x0e00'0000U,
			&Dispatcher::simd_table_lookup},
		DecodeEntry{0x9f00'fc00U, 0x0f00'0400U,
			&Dispatcher::simd_shift_immediate},
		DecodeEntry{0x9f00'fc00U, 0x0f00'2400U,
			&Dispatcher::simd_shift_immediate},
		DecodeEntry{0x9f00'fc00U, 0x0f00'4400U,
			&Dispatcher::simd_shift_immediate},
		DecodeEntry{0x9f00'fc00U, 0x0f00'5400U,
			&Dispatcher::simd_shift_immediate},
		DecodeEntry{0xdf00'fc00U, 0x5f00'0400U,
			&Dispatcher::simd_shift_immediate},
		DecodeEntry{0xdf00'fc00U, 0x5f00'2400U,
			&Dispatcher::simd_shift_immediate},
		DecodeEntry{0xdf00'fc00U, 0x5f00'4400U,
			&Dispatcher::simd_shift_immediate},
		DecodeEntry{0xdf00'fc00U, 0x5f00'5400U,
			&Dispatcher::simd_shift_immediate},
		DecodeEntry{0x9f20'fc00U, 0x0e20'4400U,
			&Dispatcher::simd_shift_register},
		DecodeEntry{0x9f20'fc00U, 0x0e20'5400U,
			&Dispatcher::simd_shift_register},
		DecodeEntry{0xff20'fc00U, 0x7e20'd400U,
			&Dispatcher::simd_floating_arithmetic},
		DecodeEntry{0x9f20'0400U, 0x0e20'0400U,
			&Dispatcher::simd_three_same},
		DecodeEntry{0xdf20'0400U, 0x5e20'0400U,
			&Dispatcher::simd_three_same},
		DecodeEntry{0x8f80'fc00U, 0x0f00'a400U,
			&Dispatcher::simd_shift_left_long},
		DecodeEntry{0xbf00'fc00U, 0x0f00'8400U,
			&Dispatcher::simd_shift_right_narrow},
		DecodeEntry{0xbf00'fc00U, 0x0f00'8c00U,
			&Dispatcher::simd_shift_right_narrow},
		DecodeEntry{0x9f00'0400U, 0x0f00'0400U,
			&Dispatcher::simd_shift_immediate},
		DecodeEntry{0xdf00'0400U, 0x5f00'0400U,
			&Dispatcher::simd_shift_immediate},
		DecodeEntry{0x9f00'0400U, 0x0f00'0000U,
			&Dispatcher::simd_by_indexed_element},
		DecodeEntry{0xdf00'0400U, 0x5f00'0000U,
			&Dispatcher::simd_by_indexed_element},
		DecodeEntry{0xbf20'0000U, 0x0c00'0000U,
			&Dispatcher::load_store_simd_structure},
		DecodeEntry{0xbf80'0000U, 0x0d00'0000U,
			&Dispatcher::load_store_simd_single_structure},
		DecodeEntry{0xbf80'0000U, 0x0d80'0000U,
			&Dispatcher::load_store_simd_single_structure},
		DecodeEntry{0x3a00'0000U, 0x2800'0000U, &Dispatcher::load_store_pair},
		DecodeEntry{0x3f00'0000U, 0x0800'0000U,
			&Dispatcher::load_store_exclusive},
		DecodeEntry{0x3b00'0000U, 0x3900'0000U, &Dispatcher::load_store_unsigned},
		DecodeEntry{0x3b00'0000U, 0x3800'0000U, &Dispatcher::load_store_other},
	};
	struct CachedDecoder final {
		std::uint32_t instruction{};
		Handler handler{};
		bool valid{};
	};
	static thread_local std::array<CachedDecoder, 4096> decoder_cache{};
	const auto cache_index = static_cast<std::size_t>(
		(instruction ^ (instruction >> 12) ^ (instruction >> 22)) &
		(decoder_cache.size() - 1));
	auto &cached = decoder_cache[cache_index];
	if (cached.valid && cached.instruction == instruction) {
		if (cached.handler != nullptr)
			return (this->*cached.handler)(cpu, memory, instruction);
		return {StopReason::unsupported_instruction, cpu.pc, instruction, 0,
			false};
	}

	for (const auto &decoder : decoders) {
		if ((instruction & decoder.mask) == decoder.value) {
			cached = {instruction, decoder.handler, true};
			return (this->*decoder.handler)(cpu, memory, instruction);
		}
	}
	cached = {instruction, nullptr, true};
	return {StopReason::unsupported_instruction, cpu.pc, instruction, 0,
		false};
}

} // namespace uml::backend::arm64
