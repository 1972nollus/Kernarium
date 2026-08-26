// SPDX-License-Identifier: GPL-2.0-or-later
#include "program_builder_decode.hpp"
#include "gadget_selection.hpp"

namespace uml::cpu::arm64::gadgets::builder_detail {

using namespace selection;

DecodeResult decode_data_processing(DecodeContext &context)
{
	const auto address = context.address;
	const auto instruction = context.instruction;
	auto &compiled = context.compiled;
	auto &words = context.words;
	const auto &options = context.options;
	const auto &resolve_link = context.resolve_link;

	if ((instruction & 0x1f80'0000U) == 0x1280'0000U) {
		const unsigned operation = (instruction >> 29) & 3U;
		const unsigned halfword = (instruction >> 21) & 3U;
		const bool is_64_bit = (instruction >> 31) != 0;
		const unsigned destination = instruction & 31U;
		if (operation == 1 ||
		    (!is_64_bit && halfword >= 2)) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else if (operation == 3U) {
			static constexpr Gadget keep[2][4]{
				{uml_arm64_gadget_move_wide_keep_w0,
				 uml_arm64_gadget_move_wide_keep_w1, nullptr, nullptr},
				{uml_arm64_gadget_move_wide_keep_x0,
				 uml_arm64_gadget_move_wide_keep_x1,
				 uml_arm64_gadget_move_wide_keep_x2,
				 uml_arm64_gadget_move_wide_keep_x3}};
			emit(words, keep[is_64_bit][halfword]);
			emit(words, destination |
				(static_cast<std::uintptr_t>(
					(instruction >> 5) & 0xffffU) << 8));
		} else {
			auto value = static_cast<std::uint64_t>(
				(instruction >> 5) & 0xffffU) << (halfword * 16U);
			if (operation == 0U)
				value = ~value;
			if (!is_64_bit)
				value &= 0xffff'ffffU;
			emit(words, uml_arm64_gadget_move_wide_constant);
			emit(words, destination * sizeof(std::uint64_t));
			emit(words, static_cast<std::uintptr_t>(value));
		}
	} else if ((instruction & 0x7ff0'0000U) == 0x11c0'0000U) {
		const unsigned destination = instruction & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			const unsigned source = (instruction >> 5) & 31U;
			const unsigned operation =
				(instruction >> 18) & 3U;
			const unsigned encoded =
				(instruction >> 10) & 0xffU;
			const bool is_64_bit =
				(instruction & 0x8000'0000U) != 0;
			const auto zero = offsetof(CpuState, gadget_zero);
			const auto source_offset = source == 31U ? zero : source * 8U;
			/* UMAX #0 is an unconditional copy. */
			if (operation == 1U && encoded == 0U) {
				if (is_64_bit && destination == source) {
					emit(words, uml_arm64_gadget_nop);
				} else {
					emit(words, is_64_bit ?
						uml_arm64_gadget_integer_min_max_copy_x :
						uml_arm64_gadget_integer_min_max_copy_w);
					emit(words,
						static_cast<std::uintptr_t>(
							destination * 8U) |
						(static_cast<std::uintptr_t>(
							source_offset)
						 << 16));
				}
			} else {
				std::uint32_t immediate = encoded;
				if ((operation & 1U) == 0U &&
				    (encoded & 0x80U) != 0)
					immediate |= 0xffff'ff00U;
				emit(words, integer_min_max_gadget(
					operation, is_64_bit, true));
				emit(words,
					static_cast<std::uintptr_t>(
						destination * 8U) |
					(static_cast<std::uintptr_t>(
						source_offset) << 16) |
					(static_cast<std::uintptr_t>(
						immediate) << 32));
			}
		}
	} else if ((instruction & 0xffc0'c000U) == 0x9180'0000U ||
		   (instruction & 0xffc0'c000U) == 0xd180'0000U) {
		emit(words, mte_tag_add_sub_gadget(
			(instruction & 0x4000'0000U) != 0,
			((instruction >> 5) & 31U) == 31U,
			(instruction & 31U) == 31U));
		emit(words, mte_tag_operand(instruction));
	} else if ((instruction & 0x1f80'0000U) == 0x1180'0000U) {
		emit(words, uml_arm64_gadget_illegal);
		emit(words, address);
		++compiled;
		return DecodeResult::terminal;
	} else if ((instruction & 0x1f80'0000U) == 0x1100'0000U) {
		const bool set_flags = (instruction & 0x2000'0000U) != 0;
		const bool subtract = (instruction & 0x4000'0000U) != 0;
		const bool wide = (instruction & 0x8000'0000U) != 0;
		const unsigned destination = instruction & 31U;
		const unsigned source = (instruction >> 5) & 31U;
		const auto immediate = static_cast<std::uintptr_t>(
			(instruction >> 10) & 0xfffU) <<
			((instruction & 0x0040'0000U) != 0 ? 12U : 0U);
		const auto operand = destination |
			(static_cast<std::uintptr_t>(source) << 8) |
			(immediate << 16);
		/* Guest CMP is SUBS with an XZR destination, and it is followed by
		 * B.cond often enough to be worth one entry rather than two. Only
		 * the plain B.cond form is fused: BC.cond shares the top byte but
		 * carries a different hint, and Rn == 31 means SP here, so both fall
		 * through to the unfused pair unchanged. A condition with no fused
		 * entry - the always forms - selects nullptr and does the same. */
		if (set_flags && subtract && destination == 31U &&
		    source != 31U && !context.following.empty() &&
		    (context.following.front() & 0xff00'0010U) == 0x5400'0000U) {
			const auto branch = context.following.front();
			if (const auto fused = compare_immediate_branch_gadget(
				    wide, branch & 15U, resolve_link != nullptr);
			    fused != nullptr) {
				const auto branch_address =
					address + sizeof(instruction);
				const auto displacement = sign_extend(
					static_cast<std::uint64_t>(
						(branch >> 5) & 0x7ffffU) << 2,
					21);
				const auto target = static_cast<std::uint64_t>(
					static_cast<std::int64_t>(branch_address) +
					displacement);
				const auto fallthrough =
					branch_address + sizeof(branch);
				/* The fall-through of the fused pair is
				 * translated inline behind it, exactly as for
				 * the unfused branch. */
				if (resolve_link &&
				    emit_inlined_packed_compare_branch(
					    context,
					    compare_immediate_branch_inline_gadget(
						    wide, branch & 15U),
					    operand, target, 40, 2)) {
					/* The loop advances past this
					 * instruction itself; the pair's second
					 * one is ours. */
					++compiled;
					return DecodeResult::decoded;
				}
				emit(words, fused);
				emit(words, operand);
				if (resolve_link) {
					emit_link(context, target);
					emit_link(context, fallthrough);
				} else {
					emit(words, target);
					emit(words, fallthrough);
				}
				/* Both guest instructions retired. */
				compiled += 2;
				return DecodeResult::terminal;
			}
		}
		emit(words, add_sub_immediate_gadget(
			wide, subtract, set_flags,
			source == 31U, destination == 31U));
		emit(words, operand);
	} else if ((instruction & 0x1f20'0000U) == 0x0b00'0000U ||
		   (instruction & 0x1fe0'0000U) == 0x0b20'0000U) {
		const bool extended = (instruction & 0x0020'0000U) != 0;
		const unsigned shift = (instruction >> 10) &
			(extended ? 0x7U : 0x3fU);
		const unsigned shift_type = (instruction >> 22) & 0x3U;
		const bool is_64_bit = (instruction >> 31) != 0;
		if ((extended && shift > 4) ||
		    (!extended && (shift_type == 3 ||
			(!is_64_bit && shift >= 32)))) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const unsigned destination = instruction & 31U;
		const unsigned left = (instruction >> 5) & 31U;
		const unsigned right = (instruction >> 16) & 31U;
		const bool set_flags = (instruction & 0x2000'0000U) != 0;
		const bool left_zero = !extended && left == 31U;
		const bool right_zero = right == 31U;
		const bool destination_discard = destination == 31U &&
			(!extended || set_flags);
		const unsigned destination_offset = destination == 31U ?
			offsetof(CpuState, sp) : destination * sizeof(std::uint64_t);
		const unsigned left_offset = left == 31U ?
			offsetof(CpuState, sp) : left * sizeof(std::uint64_t);
		const unsigned right_offset = right_zero ? 0U :
			right * sizeof(std::uint64_t);
		const unsigned transform = extended ? 0U :
			shift == 0U ? 0U : shift_type + 1U;
		const auto operand = destination_offset |
			(static_cast<std::uintptr_t>(left_offset) << 16) |
			(static_cast<std::uintptr_t>(right_offset) << 32) |
			(static_cast<std::uintptr_t>(shift) << 48);
		/* Guest CMP of two registers followed by B.cond. G116 ranks this
		 * fourth and ninth of 47,494 pairs over a full Geekbench run, 3.49%
		 * together, where the immediate form G111 fused does not reach the
		 * table. Same mechanism: the dispatch between the two entries goes,
		 * and with it the msr that reloads the flags the compare just wrote.
		 *
		 * Only the plain unshifted register-register form with an XZR
		 * destination: an extension, a shift, or a real destination leaves
		 * something the fused entry does not compute, and falls through to
		 * the unfused pair unchanged. */
		if (set_flags && (instruction & 0x4000'0000U) != 0 && !extended &&
		    shift == 0U && destination == 31U && !left_zero &&
		    !right_zero && !context.following.empty() &&
		    (context.following.front() & 0xff00'0010U) == 0x5400'0000U) {
			const auto branch = context.following.front();
			if (const auto fused = compare_register_branch_gadget(
				    is_64_bit, branch & 15U, resolve_link != nullptr);
			    fused != nullptr) {
				const auto branch_address =
					address + sizeof(instruction);
				const auto displacement = sign_extend(
					static_cast<std::uint64_t>(
						(branch >> 5) & 0x7ffffU) << 2,
					21);
				const auto target = static_cast<std::uint64_t>(
					static_cast<std::int64_t>(branch_address) +
					displacement);
				const auto fallthrough =
					branch_address + sizeof(branch);
				/* The fall-through of the fused pair is
				 * translated inline behind it, exactly as for
				 * the unfused branch. */
				if (resolve_link &&
				    emit_inlined_packed_compare_branch(
					    context,
					    compare_register_branch_inline_gadget(
						    is_64_bit, branch & 15U),
					    operand, target, 48, 2)) {
					/* The loop advances past this
					 * instruction itself; the pair's second
					 * one is ours. */
					++compiled;
					return DecodeResult::decoded;
				}
				emit(words, fused);
				emit(words, operand);
				if (resolve_link) {
					emit_link(context, target);
					emit_link(context, fallthrough);
				} else {
					emit(words, target);
					emit(words, fallthrough);
				}
				compiled += 2;
				return DecodeResult::terminal;
			}
		}
		emit(words, add_sub_register_gadget(is_64_bit,
			(instruction & 0x4000'0000U) != 0, set_flags, extended,
			transform, (instruction >> 13) & 7U, shift != 0U,
			left_zero, right_zero, destination_discard));
		emit(words, operand);
	} else if ((instruction & 0xffe0'fc00U) == 0x9ac0'1000U) {
		emit(words, mte_irg_gadget(
			((instruction >> 5) & 31U) == 31U,
			((instruction >> 16) & 31U) == 31U,
			(instruction & 31U) == 31U));
		emit(words, mte_pointer_operand(instruction));
		emit(words, reinterpret_cast<std::uintptr_t>(
			byte_popcount_table.data()));
	} else if ((instruction & 0xffe0'e000U) == 0x9a00'2000U ||
		   (instruction & 0xffe0'e000U) == 0xda00'2000U) {
		emit(words, cpa_pointer_gadget(
			(instruction & 0x4000'0000U) != 0,
			(instruction >> 10) & 7U,
			((instruction >> 5) & 31U) == 31U,
			((instruction >> 16) & 31U) == 31U,
			(instruction & 31U) == 31U));
		emit(words, cpa_pointer_operand(instruction));
	} else if ((instruction & 0xffe0'fc00U) == 0x9ac0'0000U ||
		   (instruction & 0xffe0'fc00U) == 0xbac0'0000U ||
		   (instruction & 0xffe0'fc00U) == 0x9ac0'1400U) {
		const auto opcode = instruction & 0xffe0'fc00U;
		const unsigned operation = opcode == 0x9ac0'1400U ? 0U :
			opcode == 0x9ac0'0000U ? 1U : 2U;
		emit(words, mte_pointer_gadget(operation,
			((instruction >> 5) & 31U) == 31U,
			((instruction >> 16) & 31U) == 31U,
			(instruction & 31U) == 31U));
		emit(words, mte_pointer_operand(instruction));
	} else if ((instruction & 0x1fe0'fc00U) == 0x1a00'0000U) {
		const unsigned destination = instruction & 31U;
		const unsigned left = (instruction >> 5) & 31U;
		const unsigned right = (instruction >> 16) & 31U;
		const bool set_flags =
			(instruction & 0x2000'0000U) != 0;
		if (destination == 31U && !set_flags) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			emit(words, add_sub_carry_gadget(
				(instruction & 0x8000'0000U) != 0,
				(instruction & 0x4000'0000U) != 0,
				set_flags, left == 31U, right == 31U,
				left == right && left != 31U,
				destination == 31U));
			emit(words,
				arithmetic_binary_operand(instruction));
		}
	} else if ((instruction & 0xffe0'7c10U) == 0xba00'0400U) {
		const unsigned source = (instruction >> 5) & 31U;
		const auto shifted_mask =
			static_cast<std::uintptr_t>(instruction & 15U) << 28;
		if (shifted_mask == 0U) {
			emit(words, uml_arm64_gadget_nop);
		} else if (source == 31U) {
			emit(words, uml_arm64_gadget_rmif_zero);
			emit(words, shifted_mask);
		} else {
			emit(words, uml_arm64_gadget_rmif_fast);
			emit(words,
				packed_u16_operands(
					static_cast<std::uintptr_t>(source * 8U),
					static_cast<std::uintptr_t>(
						(instruction >> 15) & 63U)) |
				(shifted_mask << 32));
		}
	} else if ((instruction & 0xffff'bc1fU) == 0x3a00'080dU) {
		const unsigned source = (instruction >> 5) & 31U;
		const auto source_offset = source == 31U ?
			offsetof(CpuState, gadget_zero) : source * 8U;
		const bool native = !options.force_software_flags &&
			host_capabilities().has(ArmFeature::flag_m);
		if ((instruction & 0x4000U) != 0)
			emit(words, native ? uml_arm64_gadget_setf_native_16 :
				uml_arm64_gadget_setf_fast_16);
		else
			emit(words, native ? uml_arm64_gadget_setf_native_8 :
				uml_arm64_gadget_setf_fast_8);
		emit(words, source_offset);
	} else if ((instruction & 0xffe0'0400U) == 0xba00'0400U ||
		   (instruction & 0xffff'0800U) == 0x3a00'0800U) {
		emit(words, uml_arm64_gadget_illegal);
		emit(words, address);
		++compiled;
		return DecodeResult::terminal;
	} else if ((instruction & 0xffe0'fc00U) == 0x9ac0'3000U) {
		if (!options.enable_pointer_authentication) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const unsigned destination = instruction & 31U;
		const unsigned source = (instruction >> 5) & 31U;
		const unsigned modifier = (instruction >> 16) & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			emit(words, source == 31U ?
				uml_arm64_gadget_pauth_pacga_z :
				uml_arm64_gadget_pauth_pacga_r);
			emit(words, pointer_auth_generic_operand(
				destination, source, modifier));
		}
	} else if ((instruction & 0x7fe0'f000U) == 0x1ac0'6000U) {
		const unsigned destination = instruction & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			const unsigned operation =
				(instruction >> 10) & 3U;
			const bool is_64_bit =
				(instruction & 0x8000'0000U) != 0;
			const auto zero = offsetof(CpuState, gadget_zero);
			const auto left_offset = left == 31U ? zero : left * 8U;
			const auto right_offset = right == 31U ? zero : right * 8U;
			std::optional<unsigned> copy_source;
			if (left == right)
				copy_source = left;
			else if (operation == 1U) {
				if (left == 31U)
					copy_source = right;
				else if (right == 31U)
					copy_source = left;
			}
			else if (operation == 3U &&
				 (left == 31U || right == 31U))
				copy_source = 31U;
			if (copy_source) {
				const auto source_offset =
					*copy_source == 31U ? zero :
						*copy_source * 8U;
				if (is_64_bit &&
				    destination == *copy_source) {
					emit(words,
						uml_arm64_gadget_nop);
				} else {
					emit(words, is_64_bit ?
						uml_arm64_gadget_integer_min_max_copy_x :
						uml_arm64_gadget_integer_min_max_copy_w);
					emit(words,
						static_cast<std::uintptr_t>(
							destination * 8U) |
						(static_cast<std::uintptr_t>(
							source_offset)
						 << 16));
				}
			} else {
				emit(words, integer_min_max_gadget(
					operation, is_64_bit, false));
				emit(words,
					static_cast<std::uintptr_t>(
						destination * 8U) |
					(static_cast<std::uintptr_t>(
						left_offset) << 16) |
					(static_cast<std::uintptr_t>(
						right_offset) << 32));
			}
		}
	} else if ((instruction & 0x5fe0'f000U) == 0x1ac0'6000U) {
		emit(words, uml_arm64_gadget_illegal);
		emit(words, address);
		++compiled;
		return DecodeResult::terminal;
	} else if ((instruction & 0xffe0'8000U) == 0x9b60'0000U ||
		   (instruction & 0xffe0'8000U) == 0x9b60'8000U) {
		emit(words, cpa_multiply_gadget(
			(instruction & 0x0000'8000U) != 0,
			((instruction >> 5) & 31U) == 31U,
			((instruction >> 16) & 31U) == 31U,
			((instruction >> 10) & 31U) == 31U,
			(instruction & 31U) == 31U));
		emit(words, cpa_multiply_operand(instruction));
	} else if ((instruction & 0x1f00'0000U) == 0x1b00'0000U) {
		const bool is_64_bit = (instruction >> 31) != 0;
		const auto operation = (instruction >> 21) & 7U;
		const bool valid = ((instruction >> 29) & 3U) == 0 &&
			(operation == 0 || (is_64_bit && (operation == 1 ||
			 operation == 2 || operation == 5 || operation == 6))) &&
			((instruction & 0x0000'8000U) == 0 ||
			 operation == 0 || operation == 1 || operation == 5) &&
			((operation != 2 && operation != 6) ||
			 ((instruction >> 10) & 0x1fU) == 0x1fU);
		if (valid) {
			const unsigned destination = instruction & 31U;
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				const unsigned left =
					(instruction >> 5) & 31U;
				const unsigned addend =
					(instruction >> 10) & 31U;
				const unsigned right =
					(instruction >> 16) & 31U;
				emit(words, multiply_add_gadget(operation,
					(instruction & 0x8000U) != 0,
					is_64_bit, left == 31U,
					right == 31U, addend == 31U));
				emit(words,
					static_cast<std::uintptr_t>(
						destination * 8U) |
					(static_cast<std::uintptr_t>(
						left * 8U) << 16) |
					(static_cast<std::uintptr_t>(
						right * 8U) << 32) |
					(static_cast<std::uintptr_t>(
						addend * 8U) << 48));
			}
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0x7fe0'f000U) == 0x1ac0'2000U) {
		const unsigned destination = instruction & 31U;
		const unsigned left = (instruction >> 5) & 31U;
		const unsigned right = (instruction >> 16) & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			emit(words, variable_shift_gadget(
				(instruction & 0x8000'0000U) != 0,
				(instruction >> 10) & 3U,
				left == 31U, right == 31U,
				left == right && left != 31U));
			emit(words,
				arithmetic_binary_operand(instruction));
		}
	} else if ((instruction & 0x7fe0'f800U) == 0x1ac0'0800U) {
		const unsigned destination = instruction & 31U;
		const unsigned left = (instruction >> 5) & 31U;
		const unsigned right = (instruction >> 16) & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			emit(words, divide_gadget(
				(instruction & 0x8000'0000U) != 0,
				(instruction & 0x400U) != 0,
				left == 31U, right == 31U,
				left == right && left != 31U));
			emit(words,
				arithmetic_binary_operand(instruction));
		}
	} else if ((instruction & 0x7fe0'e000U) == 0x1ac0'4000U) {
		const unsigned size = (instruction >> 10) & 3U;
		if (((instruction >> 31) != 0) == (size == 3U)) {
			const unsigned destination = instruction & 31U;
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			if (destination == 31U) {
				emit(words, uml_arm64_gadget_nop);
			} else {
				emit(words, crc32_gadget(
					(instruction >> 10) & 7U,
					left == 31U, right == 31U,
					left == right && left != 31U,
					!options.force_software_checksum &&
						host_capabilities().crc32));
				emit(words, arithmetic_binary_operand(instruction));
			}
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0x5fe0'0000U) == 0x1ac0'0000U) {
		/* Every allocated data-processing (2 source) member is selected
		 * above. The remaining opcode combinations are reserved.
		 */
		emit(words, uml_arm64_gadget_illegal);
		emit(words, address);
		++compiled;
		return DecodeResult::terminal;
	} else if ((instruction & 0x1f80'0000U) == 0x1380'0000U) {
		const bool is_64_bit = (instruction >> 31) != 0;
		const unsigned shift = (instruction >> 10) & 0x3fU;
		if (((instruction >> 29) & 3U) == 0U &&
		    (instruction & 0x0020'0000U) == 0U &&
		    ((instruction >> 22) & 1U) ==
			    static_cast<unsigned>(is_64_bit) &&
		    (is_64_bit || shift < 32U)) {
			const unsigned destination = instruction & 31U;
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			emit(words, extract_register_gadget(is_64_bit,
				shift != 0U, left == 31U, right == 31U,
				destination == 31U));
			emit(words, destination |
				(static_cast<std::uintptr_t>(left) << 8) |
				(static_cast<std::uintptr_t>(right) << 16) |
				(static_cast<std::uintptr_t>(shift) << 24));
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0x1f00'0000U) == 0x0a00'0000U) {
		const bool is_64_bit = (instruction >> 31) != 0;
		const unsigned shift = (instruction >> 10) & 0x3fU;
		if (!is_64_bit && shift >= 32) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const unsigned destination = instruction & 31U;
		const unsigned left = (instruction >> 5) & 31U;
		const unsigned right = (instruction >> 16) & 31U;
		const unsigned operation = (instruction >> 29) & 3U;
		if (destination == 31U && operation != 3U) {
			/* A non-flag-setting write to XZR/WZR has no effect. */
			emit(words, uml_arm64_gadget_nop);
		} else {
			const unsigned shift_kind = shift == 0U ? 0U :
				((instruction >> 22) & 3U) + 1U;
			emit(words, logical_shifted_gadget(is_64_bit,
				operation,
				(instruction & 0x0020'0000U) != 0,
				shift_kind, left == 31U, right == 31U,
				destination == 31U));
			emit(words, destination |
				(static_cast<std::uintptr_t>(left) << 8) |
				(static_cast<std::uintptr_t>(right) << 16) |
				(static_cast<std::uintptr_t>(shift) << 24));
		}
	} else if ((instruction & 0x3fe0'0000U) == 0x1a80'0000U) {
		if ((instruction & 0x0000'0800U) != 0) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const unsigned destination = instruction & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			const unsigned left = (instruction >> 5) & 31U;
			const unsigned right = (instruction >> 16) & 31U;
			const unsigned operation = ((instruction >> 29) & 2U) |
				((instruction >> 10) & 1U);
			const unsigned condition =
				(instruction >> 12) & 15U;
			const bool unconditional_copy =
				condition >= 14U ||
				(operation == 0U && left == right);
			const auto zero = offsetof(CpuState, gadget_zero);
			const auto left_offset = left == 31U ? zero : left * 8U;
			const auto right_offset = right == 31U ? zero : right * 8U;
			emit(words, conditional_select_gadget(
				unconditional_copy ? 14U : condition,
				operation,
				(instruction & 0x8000'0000U) != 0));
			emit(words,
				static_cast<std::uintptr_t>(destination * 8U) |
				(static_cast<std::uintptr_t>(left_offset) << 16) |
				(static_cast<std::uintptr_t>(
					unconditional_copy ? 0U :
						right_offset)
				 << 32));
		}
	} else if ((instruction & 0x3fe0'0000U) == 0x3a40'0000U) {
		if ((instruction & 0x0000'0410U) != 0) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const bool immediate = (instruction & 0x800U) != 0;
		const unsigned left = (instruction >> 5) & 31U;
		const unsigned right = (instruction >> 16) & 31U;
		const auto zero = offsetof(CpuState, gadget_zero);
		const auto left_operand = left == 31U ? zero : left * 8U;
		const auto right_operand = immediate ? right :
			right == 31U ? zero : right * 8U;
		emit(words, conditional_compare_gadget(
			(instruction >> 12) & 15U,
			(instruction & 0x4000'0000U) != 0,
			(instruction & 0x8000'0000U) != 0, immediate));
		emit(words, static_cast<std::uintptr_t>(left_operand) |
			(static_cast<std::uintptr_t>(right_operand) << 16) |
			(static_cast<std::uintptr_t>(instruction & 15U) << 60));
	} else if ((instruction & 0x1f80'0000U) == 0x1200'0000U) {
		const bool is_64_bit = (instruction >> 31) != 0;
		const auto immediate = detail::decode_logical_immediate(
			is_64_bit, (instruction >> 22) & 1U,
			(instruction >> 16) & 0x3fU,
			(instruction >> 10) & 0x3fU);
		if (!immediate) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const unsigned destination = instruction & 31U;
		const unsigned source = (instruction >> 5) & 31U;
		emit(words, logical_immediate_gadget(is_64_bit,
			(instruction >> 29) & 3U, source == 31U,
			destination == 31U));
		emit(words, destination |
			(static_cast<std::uintptr_t>(source) << 8));
		emit(words, *immediate);
	} else if ((instruction & 0x1f80'0000U) == 0x1300'0000U) {
		const bool is_64_bit = (instruction >> 31) != 0;
		const auto n = (instruction >> 22) & 1U;
		const auto rotate = (instruction >> 16) & 0x3fU;
		const auto imms = (instruction >> 10) & 0x3fU;
		const auto operation = (instruction >> 29) & 3U;
		const auto masks = detail::decode_bit_masks(
			is_64_bit, n, rotate, imms, false);
		if (operation == 3 || n != static_cast<unsigned>(is_64_bit) ||
		    (!is_64_bit && ((rotate | imms) & 0x20U) != 0) ||
		    !masks) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const unsigned destination = instruction & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			const unsigned source = (instruction >> 5) & 31U;
			emit(words, bitfield_gadget(is_64_bit, operation,
				rotate != 0U, source == 31U));
			emit(words,
				static_cast<std::uintptr_t>(destination * 8U) |
				(static_cast<std::uintptr_t>(source * 8U) << 16) |
				(static_cast<std::uintptr_t>(rotate) << 32) |
				(static_cast<std::uintptr_t>(imms) << 40));
			emit(words, masks->write);
			emit(words, masks->test);
		}
	} else if ((instruction & 0xffff'c000U) == 0xdac1'0000U) {
		if (!options.enable_pointer_authentication) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const unsigned encoded_operation = (instruction >> 10) & 15U;
		const bool zero_modifier = encoded_operation >= 8U;
		const unsigned modifier = (instruction >> 5) & 31U;
		const unsigned destination = instruction & 31U;
		/* PAC*Z/AUT*Z reserve every encoding whose nominal Rn field is
		 * not 11111. Reject it before applying the XZR-destination fast
		 * path so malformed guest code cannot hide invalid state. */
		if (zero_modifier && modifier != 31U) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			emit(words, pointer_auth_gadgets[encoded_operation & 7U]
				[zero_modifier]);
			emit(words, zero_modifier ?
				destination * sizeof(std::uint64_t) :
				pointer_auth_operand(destination, modifier));
		}
	} else if ((instruction & 0xffff'f800U) == 0xdac1'4000U) {
		if (!options.enable_pointer_authentication) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		/* XPACI/XPACD require Rn==11111. D and Rd are the only
		 * variable fields; discard-to-XZR is side-effect free. */
		if (((instruction >> 5) & 31U) != 31U) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		if ((instruction & 31U) == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			emit(words, (instruction & 0x400U) != 0 ?
				uml_arm64_gadget_xpacd : uml_arm64_gadget_xpaci);
			emit(words, static_cast<std::uintptr_t>(
				(instruction & 31U) * sizeof(std::uint64_t)));
		}
	} else if ((instruction & 0x7fff'8000U) == 0x5ac0'0000U) {
		const bool is_64_bit = (instruction >> 31) != 0;
		const auto operation = (instruction >> 10) & 0x1fU;
		if (operation > 8 || (!is_64_bit && operation == 3)) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const unsigned destination = instruction & 31U;
		const unsigned source = (instruction >> 5) & 31U;
		if (destination == 31U) {
			emit(words, uml_arm64_gadget_nop);
		} else {
			emit(words, bit_reverse_count_gadget(
				is_64_bit, operation));
			emit(words, packed_u16_operands(
				source == 31U ? offsetof(CpuState, gadget_zero) :
					offsetof(CpuState, registers) +
						source * sizeof(std::uint64_t),
				offsetof(CpuState, registers) +
					destination * sizeof(std::uint64_t)));
		}
	} else {
		return implementation_gap(context);
	}
	return DecodeResult::decoded;
}

} // namespace uml::cpu::arm64::gadgets::builder_detail
