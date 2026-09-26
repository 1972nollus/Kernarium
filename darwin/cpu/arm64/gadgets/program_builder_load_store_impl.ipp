// SPDX-License-Identifier: GPL-2.0-or-later
#include "program_builder_decode.hpp"
#include "gadget_selection.hpp"

namespace uml::cpu::arm64::gadgets::builder_detail {

using namespace selection;

DecodeResult decode_load_store(DecodeContext &context)
{
	const auto address = context.address;
	const auto instruction = context.instruction;
	auto &compiled = context.compiled;
	auto &program_accesses_fp_state =
		context.program_accesses_fp_state;
	auto &words = context.words;
	const auto &options = context.options;

	if (const auto selected = gcs_gadget(instruction); selected) {
		emit(words, selected->entry);
		if (selected->has_operand)
			emit(words, selected->operand);
		emit(words, address);
		return DecodeResult::decoded;
	}
	if ((instruction & 0xff00'0000U) == 0xd800'0000U) {
		/* PRFM (literal) is an implementation-defined performance hint. */
		emit(words, uml_arm64_gadget_nop);
	} else if ((instruction & 0x3b00'0000U) == 0x1800'0000U) {
		const bool vector = (instruction & 0x0400'0000U) != 0;
		const unsigned operation = instruction >> 30;
		if (vector && operation == 3U) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const unsigned destination = instruction & 31U;
		emit(words, literal_load_gadget(
			vector, operation, !vector && destination == 31U));
		const auto destination_offset = vector ?
			offsetof(CpuState, vectors) +
				destination * sizeof(VectorRegister) :
			destination < 31U ? destination * sizeof(std::uint64_t) : 0U;
		const auto displacement =
			sign_extend((instruction >> 5) & 0x7ffffU, 19) * 4;
		emit(words, destination_offset |
			(static_cast<std::uint64_t>(displacement) << 16));
		emit(words, address);
	} else if ((instruction & 0xbfbf'fc00U) == 0x0d01'8400U) {
		const bool load = (instruction & 0x0040'0000U) != 0U;
		const unsigned lane = instruction >> 30;
		const bool sp_base = ((instruction >> 5) & 31U) == 31U;
		emit(words, simd_ordered_lane_gadgets[load][lane][sp_base]);
		emit(words, simd_ordered_memory_operand(instruction));
		emit(words, address);
	} else if ((instruction & 0x3f20'0c00U) == 0x1d00'0800U) {
		const unsigned size = instruction >> 30;
		const unsigned opc = (instruction >> 22) & 3U;
		if ((opc & 2U) != 0U && size != 0U) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const bool load = (opc & 1U) != 0U;
		const bool sp_base = ((instruction >> 5) & 31U) == 31U;
		if ((opc & 2U) != 0U) {
			const unsigned native = host_has_lrcpc3();
			emit(words, (load ? simd_ordered_q_load_gadgets :
				simd_ordered_q_store_gadgets)[native][sp_base]);
		} else {
			emit(words, (load ? simd_ordered_scalar_load_gadgets :
				simd_ordered_scalar_store_gadgets)[size][sp_base]);
		}
		const auto offset = sign_extend(
			(instruction >> 12) & 0x1ffU, 9);
		emit(words, simd_ordered_memory_operand(instruction, offset));
		emit(words, address);
	} else if ((instruction & 0xbf20'0000U) == 0x0c00'0000U) {
		const unsigned opcode = (instruction >> 12) & 15U;
		const bool interleaved = opcode == 8U || opcode == 4U ||
			opcode == 0U;
		const bool valid_opcode = interleaved || opcode == 7U ||
			opcode == 10U || opcode == 6U || opcode == 2U;
		const bool post_index = (instruction & 0x0080'0000U) != 0;
		const bool valid_no_offset = post_index ||
			((instruction >> 16) & 31U) == 0U;
		const bool valid_arrangement = !interleaved ||
			(instruction & 0x4000'0c00U) != 0x0000'0c00U;
		if (valid_opcode && valid_no_offset && valid_arrangement) {
			const bool load =
				(instruction & 0x0040'0000U) != 0;
			const bool q = (instruction & 0x4000'0000U) != 0;
			const unsigned size = (instruction >> 10) & 3U;
			const unsigned base = (instruction >> 5) & 31U;
			const unsigned offset_register =
				(instruction >> 16) & 31U;
			const unsigned address_mode = !post_index ? 0U :
				offset_register == 31U ? 1U : 2U;
			const auto *gadgets =
				simd_structure_gadgets(opcode, q, size);
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			const auto offset_register_offset = offset_register < 31U ?
				offset_register * sizeof(std::uint64_t) : 0U;
			emit(words, (load ? gadgets->load : gadgets->store)
				[base == 31U][address_mode]);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>(base_offset) << 16) |
				(static_cast<std::uintptr_t>(offset_register_offset)
					<< 32) |
				(static_cast<std::uintptr_t>(
					std::size_t{1} << size) << 48));
			emit(words, address);
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0xbf80'0000U) == 0x0d00'0000U ||
		   (instruction & 0xbf80'0000U) == 0x0d80'0000U) {
		const unsigned opcode = (instruction >> 13) & 7U;
		const unsigned scale = opcode >> 1;
		const bool load = (instruction & 0x0040'0000U) != 0;
		const bool scale_bit = (instruction & 0x0000'1000U) != 0;
		const unsigned size = (instruction >> 10) & 3U;
		const bool post_index = (instruction & 0x0080'0000U) != 0;
		const bool valid_no_offset = post_index ||
			((instruction >> 16) & 31U) == 0U;
		const bool replicate = scale == 3U && load && !scale_bit;
		const bool valid_lane = scale == 0U ||
			(scale == 1U && (size & 1U) == 0U) ||
			(scale == 2U && (size & 2U) == 0U &&
			 ((size & 1U) == 0U || !scale_bit));
		if (valid_no_offset && replicate) {
			const bool q = (instruction & 0x4000'0000U) != 0;
			const unsigned count =
				((instruction & 0x0020'0000U) != 0 ? 1U : 0U) +
				(opcode == 6U ? 1U : 3U);
			const unsigned base = (instruction >> 5) & 31U;
			const unsigned offset_register =
				(instruction >> 16) & 31U;
			const unsigned address_mode = !post_index ? 0U :
				offset_register == 31U ? 1U : 2U;
			const auto &gadgets = simd_replicate_gadgets(size, q);
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			const auto offset_register_offset = offset_register < 31U ?
				offset_register * sizeof(std::uint64_t) : 0U;
			emit(words, gadgets[count - 1U][base == 31U]
				[address_mode]);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>(base_offset) << 16) |
				(static_cast<std::uintptr_t>(offset_register_offset)
					<< 32));
			emit(words, address);
		} else if (valid_no_offset && scale != 3U && valid_lane) {
			const bool q = (instruction & 0x4000'0000U) != 0;
			const unsigned count =
				((instruction & 0x0020'0000U) != 0 ? 1U : 0U) +
				((opcode & 1U) != 0 ? 3U : 1U);
			const unsigned width_index = scale < 2U ? scale :
				size == 0U ? 2U : 3U;
			const unsigned lane = scale == 0U ?
				(static_cast<unsigned>(q) << 3) |
				(static_cast<unsigned>(scale_bit) << 2) | size :
				scale == 1U ?
				(static_cast<unsigned>(q) << 2) |
				(static_cast<unsigned>(scale_bit) << 1) | (size >> 1) :
				(static_cast<unsigned>(q) << (1U - size)) |
				static_cast<unsigned>(scale_bit);
			const unsigned lane_offset = lane << width_index;
			const unsigned base = (instruction >> 5) & 31U;
			const unsigned offset_register =
				(instruction >> 16) & 31U;
			const unsigned address_mode = !post_index ? 0U :
				offset_register == 31U ? 1U : 2U;
			const auto &gadgets = simd_lane_gadgets(width_index);
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			const auto offset_register_offset = offset_register < 31U ?
				offset_register * sizeof(std::uint64_t) : 0U;
			emit(words, (load ? gadgets[count - 1U].load :
				gadgets[count - 1U].store)[base == 31U]
				[address_mode]);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>(base_offset) << 16) |
				(static_cast<std::uintptr_t>(offset_register_offset)
					<< 32) |
				(static_cast<std::uintptr_t>(lane_offset) << 48));
			emit(words, address);
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0xff20'0400U) == 0xf820'0400U) {
		if (!options.enable_pointer_authentication) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const unsigned destination = instruction & 31U;
		const unsigned base = (instruction >> 5) & 31U;
		const bool writeback = (instruction & 0x800U) != 0;
		if (writeback && base == destination && base != 31U) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const auto encoded_offset = ((instruction >> 12) & 0x1ffU) |
			(((instruction >> 22) & 1U) << 9);
		const auto displacement = sign_extend(encoded_offset, 10) * 8;
		emit(words, pointer_auth_load_gadget(
			(instruction & 0x0080'0000U) != 0, writeback,
			base == 31U, destination == 31U));
		emit(words, pointer_auth_load_operand(
			destination, base, displacement));
		emit(words, address);
	} else if ((instruction & 0xffc0'0000U) == 0xb940'0000U ||
		   (instruction & 0xffc0'0000U) == 0xf940'0000U ||
		   (instruction & 0xffc0'0000U) == 0xb900'0000U ||
		   (instruction & 0xffc0'0000U) == 0xf900'0000U) {
		const auto opcode = instruction & 0xffc0'0000U;
		const bool transfer_zero = (instruction & 31U) == 31U;
		const auto reg = instruction & 31U;
		const auto base = (instruction >> 5) & 31U;
		const auto scale = opcode == 0xb940'0000U ||
			opcode == 0xb900'0000U ? 2U : 3U;
		const auto operation = opcode == 0xb940'0000U ||
			opcode == 0xf940'0000U ? 1U : 0U;
		Gadget transfer = nullptr;
		if (!transfer_zero) {
			transfer = scalar_unsigned_memory_inplace_gadget(
				scale, operation, base == 31U, reg);
		}
		if (transfer == nullptr) {
			transfer = scalar_unsigned_memory_gadget(
				scale, operation, base == 31U, transfer_zero);
		}
		emit(words, transfer);
		const auto offset = ((instruction >> 10) & 0xfffU) << scale;
		const auto reg_offset = reg < 31U ?
			reg * sizeof(std::uint64_t) : 0U;
		const auto base_offset = base < 31U ?
			base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
		emit(words, reg_offset |
			(static_cast<std::uintptr_t>(base_offset) << 16) |
			(static_cast<std::uintptr_t>(offset) << 32) |
			fault_position(context));
	} else if ((instruction & 0x3f20'0c00U) == 0x1900'0800U &&
		   (instruction & 0x3fbf'fc00U) != 0x1980'0800U) {
		const auto fixed = instruction & 0xffe0'fc00U;
		const bool lrcpc3 = fixed == 0x9900'0800U ||
			fixed == 0xd900'0800U || fixed == 0x9900'1800U ||
			fixed == 0xd900'1800U || fixed == 0x9940'0800U ||
			fixed == 0xd940'0800U || fixed == 0x9940'1800U ||
			fixed == 0xd940'1800U;
		const bool lscp = fixed == 0xd900'5800U ||
			fixed == 0xd940'5800U || fixed == 0xd940'7800U;
		if (!lrcpc3 && !lscp) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const bool load = fixed != 0x9900'0800U &&
			fixed != 0xd900'0800U && fixed != 0x9900'1800U &&
			fixed != 0xd900'1800U && fixed != 0xd900'5800U;
		const bool writeback = lrcpc3 &&
			(instruction & 0x0000'1000U) == 0;
		const unsigned first = instruction & 31U;
		const unsigned second = (instruction >> 16) & 31U;
		if (load && first == second && first != 31U) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const unsigned base = (instruction >> 5) & 31U;
		const unsigned register_mode = (first == 31U ? 2U : 0U) |
			(second == 31U ? 1U : 0U);
		if (lscp) {
			const auto &gadgets = fixed == 0xd940'5800U ?
				consistent_pair_ldap_gadgets :
				fixed == 0xd940'7800U ?
				consistent_pair_ldapp_gadgets :
				consistent_pair_store_gadgets;
			emit(words, gadgets[register_mode]);
			emit(words, ordered_pair_operand(instruction));
			emit(words, address);
		} else {
			const unsigned size = (instruction >> 30) - 2U;
			/* LDIAPP post-index overlap selects WBSUPPRESS, which is the
			 * no-writeback gadget operating at the original address. */
			const bool effective_writeback = writeback &&
				(!load || base == 31U ||
				 (base != first && base != second));
			emit(words, load ?
				ordered_pair_load_gadgets[size][effective_writeback]
					[register_mode] :
				ordered_pair_store_gadgets[size][writeback]
					[register_mode]);
			emit(words, ordered_pair_operand(instruction));
			emit(words, address);
		}
	} else if ((instruction & 0x3fff'fc00U) == 0x38bf'c000U ||
		   (instruction & 0x3f20'0c00U) == 0x1900'0000U) {
		const bool no_offset =
			(instruction & 0x3fff'fc00U) == 0x38bf'c000U;
		const unsigned size = instruction >> 30;
		const unsigned operation = no_offset ? 1U :
			(instruction >> 22) & 3U;
		const bool valid = no_offset ||
			!((operation == 2U && size == 3U) ||
			  (operation == 3U && size >= 2U));
		if (!valid) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const auto offset = no_offset ? std::int64_t{0} :
			sign_extend((instruction >> 12) & 0x1ffU, 9);
		const unsigned transfer = instruction & 31U;
		if (operation == 0U) {
			emit(words, rcpc_store_gadgets[size][transfer == 31U]);
		} else {
			unsigned variant = size;
			if (operation == 2U)
				variant = size == 0U ? 5U :
					size == 1U ? 7U : 8U;
			else if (operation == 3U)
				variant = size == 0U ? 4U : 6U;
			emit(words,
				rcpc_load_gadgets[variant][transfer == 31U]);
		}
		emit(words, rcpc_memory_operand(instruction, offset));
		emit(words, address);
	} else if ((instruction & 0x3fbf'fc00U) == 0x1980'0800U) {
		if ((instruction >> 30) >= 2U) {
			const bool load =
				(instruction & 0x0040'0000U) != 0;
			const bool wide = (instruction >> 30) == 3U;
			const unsigned transfer = instruction & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			const bool suppress = load && base != 31U && base == transfer;
			Gadget gadget{};
			if (load) {
				if (wide)
					gadget = suppress ?
						uml_arm64_gadget_ordered_writeback_load_x_suppress :
						transfer == 31U ?
						uml_arm64_gadget_ordered_writeback_load_x_z :
						uml_arm64_gadget_ordered_writeback_load_x_r;
				else
					gadget = suppress ?
						uml_arm64_gadget_ordered_writeback_load_w_suppress :
						transfer == 31U ?
						uml_arm64_gadget_ordered_writeback_load_w_z :
						uml_arm64_gadget_ordered_writeback_load_w_r;
			} else if (wide) {
				gadget = transfer == 31U ?
					uml_arm64_gadget_ordered_writeback_store_x_z :
					uml_arm64_gadget_ordered_writeback_store_x_r;
			} else {
				gadget = transfer == 31U ?
					uml_arm64_gadget_ordered_writeback_store_w_z :
					uml_arm64_gadget_ordered_writeback_store_w_r;
			}
			const auto transfer_offset = transfer < 31U ?
				transfer * sizeof(std::uint64_t) : 0U;
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			emit(words, gadget);
			emit(words, transfer_offset |
				(static_cast<std::uintptr_t>(base_offset) << 16));
			emit(words, address);
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0x3fa0'0000U) == 0x0880'0000U) {
		const bool valid = ((instruction >> 16) & 31U) == 31U &&
			((instruction >> 10) & 31U) == 31U;
		if (valid) {
			const bool load =
				(instruction & 0x0040'0000U) != 0;
			const unsigned ordering =
				(instruction & 0x0000'8000U) != 0;
			const unsigned size = instruction >> 30;
			const unsigned transfer = instruction & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			emit(words, load ?
				ordered_load_gadgets[ordering][size]
					[transfer == 31U] :
				ordered_store_gadgets[ordering][size]
					[transfer == 31U]);
			const auto transfer_offset = transfer < 31U ?
				transfer * sizeof(std::uint64_t) : 0U;
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			emit(words, transfer_offset |
				(static_cast<std::uintptr_t>(base_offset) << 16));
			emit(words, address);
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0xbf20'fc00U) == 0x1920'0800U ||
		   ((instruction & 0xbf20'8c00U) == 0x3820'8000U &&
		    ((instruction >> 12) & 7U) >= 1U &&
		    ((instruction >> 12) & 7U) <= 3U)) {
		const bool compare_swap =
			(instruction & 0xbf20'fc00U) == 0x1920'0800U;
		const unsigned encoded_operation =
			(instruction >> 12) & 7U;
		const bool valid = compare_swap ||
			(encoded_operation >= 1U && encoded_operation <= 3U);
		if (!valid) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const unsigned first = (instruction >> 16) & 31U;
		const unsigned second = instruction & 31U;
		const unsigned source = compare_swap ? second : first;
		const unsigned result = compare_swap ? first : second;
		const bool acquire =
			(instruction & 0x0080'0000U) != 0 && result != 31U;
		const bool release =
			(instruction & 0x0040'0000U) != 0;
		const unsigned ordering =
			(acquire ? 2U : 0U) | (release ? 1U : 0U);
		const unsigned registers = (source == 31U ? 2U : 0U) |
			(result == 31U ? 1U : 0U);
		const unsigned operation = compare_swap ? 0U :
			encoded_operation;
		const unsigned soft = (instruction >> 30) & 1U;
		const unsigned base = (instruction >> 5) & 31U;
		if (base == 31U)
			emit(words,
				uml_arm64_gadget_atomic_sp_alignment_write);
		emit(words, rcw_scalar_gadgets[operation][soft][ordering]
			[registers]);
		emit(words, rcw_scalar_operand(instruction, compare_swap));
		emit(words, address);
	} else if ((instruction & 0xbf20'fc00U) == 0x1920'0c00U ||
		   ((instruction & 0xbf20'8c00U) == 0x1920'8000U &&
		    (((instruction >> 12) & 7U) != 0U ||
		     (instruction & 0x4000'0000U) != 0U))) {
		const bool compare_swap =
			(instruction & 0xbf20'fc00U) == 0x1920'0c00U;
		const unsigned operation = (instruction >> 12) & 7U;
		const unsigned first = instruction & 31U;
		const unsigned second = (instruction >> 16) & 31U;
		const bool valid_operation = compare_swap ||
			(operation >= 1U && operation <= 3U);
		const bool valid_registers = compare_swap ?
			(first & 1U) == 0 && (second & 1U) == 0 :
			first != 31U && second != 31U && first != second;
		if (!valid_operation || !valid_registers) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const bool acquire =
			(instruction & 0x0080'0000U) != 0;
		const bool release =
			(instruction & 0x0040'0000U) != 0;
		const unsigned ordering =
			(acquire ? 2U : 0U) | (release ? 1U : 0U);
		const unsigned soft = (instruction >> 30) & 1U;
		const unsigned base = (instruction >> 5) & 31U;
		if (base == 31U)
			emit(words,
				uml_arm64_gadget_atomic_sp_alignment_write);
		if (compare_swap) {
			const unsigned registers =
				(first == 30U ? 2U : 0U) |
				(second == 30U ? 1U : 0U);
			emit(words,
				rcw_pair_cas_gadgets[soft][ordering][registers]);
		} else {
			emit(words, rcw_pair_modify_gadgets[operation - 1U]
				[soft][ordering]);
		}
		emit(words, rcw_pair_operand(instruction));
		emit(words, address);
	} else if ((instruction & 0xbf20'0c00U) == 0x1920'0000U) {
		const unsigned first = instruction & 31U;
		const unsigned second = (instruction >> 16) & 31U;
		const unsigned o3 = (instruction >> 15) & 1U;
		const unsigned opcode = (instruction >> 12) & 7U;
		const unsigned operation = o3 ? 8U : opcode;
		const bool valid_operation =
			(instruction & 0x4000'0000U) == 0U &&
			(!o3 || opcode == 0U) &&
			(operation == 1U || operation == 3U || operation == 8U);
		const bool valid_registers = first != 31U && second != 31U &&
			first != second;
		if (valid_operation && valid_registers) {
			const bool acquire =
				(instruction & 0x0080'0000U) != 0;
			const bool release =
				(instruction & 0x0040'0000U) != 0;
			const unsigned ordering =
				(acquire ? 2U : 0U) | (release ? 1U : 0U);
			const unsigned family = operation == 1U ? 0U :
				operation == 3U ? 1U : 2U;
			emit(words, atomic_pair128_gadgets[family][ordering]);
			const unsigned base = (instruction >> 5) & 31U;
			const auto first_offset = first * sizeof(std::uint64_t);
			const auto second_offset = second * sizeof(std::uint64_t);
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			emit(words, first_offset |
				(static_cast<std::uintptr_t>(second_offset) << 16) |
				(static_cast<std::uintptr_t>(base_offset) << 32));
			emit(words, address);
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0x3f20'0c00U) == 0x1900'0400U ||
		   (instruction & 0x3f20'0c00U) == 0x1d00'0400U ||
		   (instruction & 0x3fe0'0c00U) == 0x19c0'0400U ||
		   (instruction & 0x3fe0'0c00U) == 0x1dc0'0400U) {
		/* SET and SETG occupy values also accepted by the broad CPYF/CPY
		 * masks. Resolve both specific families before selecting CPY. */
		const bool set_tag =
			(instruction & 0x3fe0'0c00U) == 0x1dc0'0400U;
		const bool set = !set_tag &&
			(instruction & 0x3fe0'0c00U) == 0x19c0'0400U;
		const bool copy_forward = !set && !set_tag &&
			(instruction & 0x3f20'0c00U) == 0x1900'0400U;
		const unsigned family = set ? 2U : copy_forward ? 0U : 1U;
		const unsigned destination = instruction & 31U;
		const unsigned count = (instruction >> 5) & 31U;
		const unsigned source = (instruction >> 16) & 31U;
		const unsigned stage = (set || set_tag) ?
			(instruction >> 14) & 3U :
			(instruction >> 22) & 3U;
		const bool valid = (instruction >> 30) == 0 && stage != 3 &&
			destination != 31U && count != 31U &&
			destination != count && source != destination &&
			source != count && (set || set_tag || source != 31U);
		if (!valid) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		emit(words, set_tag ?
			mops_set_tag_gadget(stage, source == 31U) :
			mops_gadget(family, stage, source == 31U));
		if (stage != 1) {
			emit(words, mops_operand(instruction));
			if (set_tag || stage == 2)
				emit(words, address);
		}
	} else if ((instruction & 0xffff'fc00U) == 0xd9e0'0000U ||
		   (instruction & 0xffff'fc00U) == 0xd9a0'0000U ||
		   (instruction & 0xffff'fc00U) == 0xd920'0000U) {
		/* LDGM/STGM/STZGM are undefined at EL0. Reject them before
		 * observing CpuState or memory.
		 */
		emit(words, uml_arm64_gadget_illegal);
		emit(words, address);
		++compiled;
		return DecodeResult::terminal;
	} else if ((instruction & 0xffc0'0000U) == 0x6880'0000U ||
		   (instruction & 0xffc0'0000U) == 0x6900'0000U ||
		   (instruction & 0xffc0'0000U) == 0x6980'0000U) {
		const unsigned encoded_mode = (instruction >> 23) & 3U;
		const unsigned mode = encoded_mode - 1U;
		const unsigned first = instruction & 31U;
		const unsigned second = (instruction >> 10) & 31U;
		const unsigned base = (instruction >> 5) & 31U;
		const auto immediate = static_cast<std::int16_t>(
			detail::sign_extend((instruction >> 15) & 0x7fU, 7) * 16);
		emit(words, mte_store_pair_tag_gadget(mode, base == 31U,
			first == 31U, second == 31U));
		emit(words, mte_memory_operand(
			base, first, second, immediate, false));
		emit(words, address);
	} else if ((instruction & 0xff20'0000U) == 0xd920'0000U) {
		const unsigned operation = (instruction >> 22) & 3U;
		const unsigned encoded_mode = (instruction >> 10) & 3U;
		const unsigned transfer = instruction & 31U;
		const unsigned base = (instruction >> 5) & 31U;
		const auto immediate = static_cast<std::int16_t>(
			detail::sign_extend((instruction >> 12) & 0x1ffU, 9) * 16);
		if (encoded_mode == 0) {
			if (operation != 1U) {
				emit(words, uml_arm64_gadget_illegal);
				emit(words, address);
				++compiled;
				return DecodeResult::terminal;
			}
			emit(words, mte_load_tag_gadget(
				base == 31U, transfer == 31U));
			emit(words, mte_memory_operand(
				base, transfer, 31U, immediate, false));
		} else {
			emit(words, mte_tag_store_gadget(operation,
				encoded_mode - 1U, base == 31U));
			emit(words, mte_memory_operand(
				base, transfer, 31U, immediate, true));
		}
		emit(words, address);
	} else if (const auto atomic_floating =
			detail::decode_atomic_floating(instruction);
		   atomic_floating.recognized) {
		if (!atomic_floating.valid) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const unsigned size = instruction >> 30;
		const unsigned operation =
			static_cast<unsigned>(atomic_floating.operation);
		const unsigned variant = atomic_floating.return_old ?
			(atomic_floating.acquire ? 2U : 0U) |
				(atomic_floating.release ? 1U : 0U) :
			atomic_floating.release ? 5U : 4U;
		const unsigned base = (instruction >> 5) & 31U;
		const auto &gadgets = host_has_lsfe() ?
			atomic_floating_native_gadgets :
			atomic_floating_software_gadgets;
		emit(words, gadgets[size][operation][base == 31U][variant]);
		emit(words, detail::atomic_floating_operand(instruction));
		emit(words, address);
		program_accesses_fp_state = true;
	} else if ((instruction & 0xffff'fc00U) == 0xf83f'd000U ||
		   (instruction & 0xffff'fc00U) == 0xf83f'9000U ||
		   (instruction & 0xffe0'fc00U) == 0xf820'b000U ||
		   (instruction & 0xffe0'fc00U) == 0xf820'a000U) {
		const unsigned first = instruction & 31U;
		if ((first & 1U) != 0 || first >= 24) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
		const auto fixed = instruction & 0xffe0'fc00U;
		const unsigned operation =
			(instruction & 0xffff'fc00U) == 0xf83f'd000U ? 0U :
			(instruction & 0xffff'fc00U) == 0xf83f'9000U ? 1U :
			fixed == 0xf820'b000U ? 2U : 3U;
		const unsigned base = (instruction >> 5) & 31U;
		const unsigned status = (instruction >> 16) & 31U;
		emit(words, ls64_gadget(
			operation, base == 31U, status == 31U));
		emit(words, ls64_operand(first, base, status,
			base == 31U, status == 31U));
		emit(words, address);
	} else if ((instruction & 0x3f20'0c00U) == 0x3820'0000U ||
		   (instruction & 0x3f20'0c00U) == 0x1920'0400U) {
		const bool unprivileged =
			(instruction & 0x0100'0000U) != 0;
		const unsigned encoded_size = instruction >> 30;
		const unsigned o3 = (instruction >> 15) & 1U;
		const unsigned opcode = (instruction >> 12) & 7U;
		const bool supported = unprivileged ?
			(encoded_size <= 1U &&
			 ((!o3 && (opcode == 0U || opcode == 1U ||
				     opcode == 3U)) ||
			  (o3 && opcode == 0U))) :
			(!o3 || opcode == 0U);
		if (supported) {
			const unsigned operation = o3 ? 8U : opcode;
			const unsigned size = unprivileged ?
				encoded_size + 2U : encoded_size;
			const unsigned source = (instruction >> 16) & 31U;
			const unsigned destination = instruction & 31U;
			const bool acquire =
				(instruction & 0x0080'0000U) != 0 &&
				destination != 31U;
			const bool release =
				(instruction & 0x0040'0000U) != 0;
			const unsigned ordering =
				(acquire ? 2U : 0U) | (release ? 1U : 0U);
			const unsigned registers =
				(source == 31U ? 2U : 0U) |
				(destination == 31U ? 1U : 0U);
			const unsigned base = (instruction >> 5) & 31U;
			if (base == 31U)
				emit(words,
					uml_arm64_gadget_atomic_sp_alignment_write);
			emit(words, atomic_rmw_gadgets[operation][ordering]
				[size][registers]);
			const auto source_offset = source < 31U ?
				source * sizeof(std::uint64_t) : 0U;
			const auto destination_offset = destination < 31U ?
				destination * sizeof(std::uint64_t) : 0U;
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			emit(words, destination_offset |
				(static_cast<std::uintptr_t>(source_offset) << 16) |
				(static_cast<std::uintptr_t>(base_offset) << 32));
			emit(words, address);
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0xbfa0'0000U) == 0x0820'0000U ||
		   (instruction & 0xbfa0'0000U) == 0x0980'0000U) {
		const unsigned compare = (instruction >> 16) & 31U;
		const unsigned replacement = instruction & 31U;
		const bool unprivileged = (instruction & 0x0100'0000U) != 0;
		const unsigned size_code = instruction >> 30;
		const bool valid = ((instruction >> 10) & 31U) == 31U &&
			(compare & 1U) == 0 && (replacement & 1U) == 0 &&
			size_code <= 1U && (!unprivileged || size_code == 1U);
		if (valid) {
			const bool acquire =
				(instruction & 0x0040'0000U) != 0;
			const bool release =
				(instruction & 0x0000'8000U) != 0;
			const unsigned ordering =
				(acquire ? 2U : 0U) | (release ? 1U : 0U);
			const unsigned registers =
				(compare == 30U ? 2U : 0U) |
				(replacement == 30U ? 1U : 0U);
			const unsigned base = (instruction >> 5) & 31U;
			if (base == 31U)
				emit(words,
					uml_arm64_gadget_atomic_sp_alignment_write);
			emit(words, compare_swap_pair_gadgets[ordering]
				[size_code][registers]);
			const auto compare_offset = compare * sizeof(std::uint64_t);
			const auto replacement_offset =
				replacement * sizeof(std::uint64_t);
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			emit(words, compare_offset |
				(static_cast<std::uintptr_t>(replacement_offset) << 16) |
				(static_cast<std::uintptr_t>(base_offset) << 32));
			emit(words, address);
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0x3fa0'0000U) == 0x08a0'0000U ||
		   (instruction & 0xbfa0'0000U) == 0x8980'0000U) {
		const bool unprivileged = (instruction & 0x0100'0000U) != 0;
		const bool valid = ((instruction >> 10) & 31U) == 31U &&
			(!unprivileged || (instruction >> 30) == 3U);
		if (valid) {
			const unsigned compare = (instruction >> 16) & 31U;
			const unsigned replacement = instruction & 31U;
			const bool acquire =
				(instruction & 0x0040'0000U) != 0 &&
				compare != 31U;
			const bool release =
				(instruction & 0x0000'8000U) != 0;
			const unsigned ordering =
				(acquire ? 2U : 0U) | (release ? 1U : 0U);
			const unsigned registers =
				(compare == 31U ? 2U : 0U) |
				(replacement == 31U ? 1U : 0U);
			const unsigned base = (instruction >> 5) & 31U;
			if (base == 31U)
				emit(words,
					uml_arm64_gadget_atomic_sp_alignment_write);
			emit(words, compare_swap_gadgets[ordering]
				[instruction >> 30][registers]);
			const auto compare_offset = compare < 31U ?
				compare * sizeof(std::uint64_t) : 0U;
			const auto replacement_offset = replacement < 31U ?
				replacement * sizeof(std::uint64_t) : 0U;
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			emit(words, compare_offset |
				(static_cast<std::uintptr_t>(replacement_offset) << 16) |
				(static_cast<std::uintptr_t>(base_offset) << 32));
			emit(words, address);
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0xbfa0'0000U) == 0x8820'0000U) {
		const bool load = (instruction & 0x0040'0000U) != 0;
		const unsigned first = instruction & 31U;
		const unsigned second = (instruction >> 10) & 31U;
		const unsigned status = (instruction >> 16) & 31U;
		const bool valid = !load ||
			(status == 31U && first != second);
		if (valid) {
			const unsigned ordering =
				(instruction & 0x0000'8000U) != 0;
			const unsigned size = (instruction >> 30) - 2U;
			const unsigned base = (instruction >> 5) & 31U;
			if (base == 31U)
				emit(words, load ?
					uml_arm64_gadget_atomic_sp_alignment_read :
					uml_arm64_gadget_atomic_sp_alignment_write);
			if (load) {
				const unsigned registers =
					(first == 31U ? 2U : 0U) |
					(second == 31U ? 1U : 0U);
				emit(words, exclusive_pair_load_gadgets
					[ordering][size][registers]);
				const auto first_offset = first < 31U ?
					first * sizeof(std::uint64_t) : 0U;
				const auto second_offset = second < 31U ?
					second * sizeof(std::uint64_t) : 0U;
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				emit(words, first_offset |
					(static_cast<std::uintptr_t>(second_offset) << 16) |
					(static_cast<std::uintptr_t>(base_offset) << 32));
			} else {
				const unsigned registers =
					(first == 31U ? 4U : 0U) |
					(second == 31U ? 2U : 0U) |
					(status == 31U ? 1U : 0U);
				emit(words, exclusive_pair_store_gadgets
					[ordering][size][registers]);
				const auto first_offset = first < 31U ?
					first * sizeof(std::uint64_t) : 0U;
				const auto second_offset = second < 31U ?
					second * sizeof(std::uint64_t) : 0U;
				const auto status_offset = status < 31U ?
					status * sizeof(std::uint64_t) : 0U;
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				emit(words, first_offset |
					(static_cast<std::uintptr_t>(second_offset) << 16) |
					(static_cast<std::uintptr_t>(status_offset) << 32) |
					(static_cast<std::uintptr_t>(base_offset) << 48));
			}
			emit(words, address);
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0x3fa0'0000U) == 0x0800'0000U ||
		   (instruction & 0xbfa0'0000U) == 0x8900'0000U) {
		const bool load = (instruction & 0x0040'0000U) != 0;
		const unsigned transfer = instruction & 31U;
		const unsigned status = (instruction >> 16) & 31U;
		const bool valid = ((instruction >> 10) & 31U) == 31U &&
			(!load || status == 31U);
		if (valid) {
			const unsigned ordering =
				(instruction & 0x0000'8000U) != 0;
			const unsigned size = instruction >> 30;
			const unsigned base = (instruction >> 5) & 31U;
			if (base == 31U)
				emit(words, load ?
					uml_arm64_gadget_atomic_sp_alignment_read :
					uml_arm64_gadget_atomic_sp_alignment_write);
			if (load) {
				emit(words, exclusive_load_gadgets[ordering]
					[size][transfer == 31U]);
				const auto transfer_offset = transfer < 31U ?
					transfer * sizeof(std::uint64_t) : 0U;
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16));
			} else {
				const unsigned registers =
					(transfer == 31U ? 2U : 0U) |
					(status == 31U ? 1U : 0U);
				emit(words, exclusive_store_gadgets[ordering]
					[size][registers]);
				const auto transfer_offset = transfer < 31U ?
					transfer * sizeof(std::uint64_t) : 0U;
				const auto status_offset = status < 31U ?
					status * sizeof(std::uint64_t) : 0U;
				const auto base_offset = base < 31U ?
					base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(status_offset) << 16) |
					(static_cast<std::uintptr_t>(base_offset) << 32));
			}
			emit(words, address);
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0x3b00'0000U) == 0x3900'0000U &&
		   (instruction & 0x0400'0000U) == 0) {
		const auto size_code = instruction >> 30;
		const auto operation = (instruction >> 22) & 3U;
		if (size_code == 3 && operation == 2) {
			/* PRFM is architecturally only a hint. */
			emit(words, uml_arm64_gadget_nop);
		} else if ((size_code >= 2 && operation == 3) ||
			   (size_code == 3 && operation > 1)) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		} else {
			const unsigned transfer = instruction & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			emit(words, scalar_unsigned_memory_gadget(size_code,
				operation, base == 31U, transfer == 31U));
			const auto transfer_offset = transfer < 31U ?
				transfer * sizeof(std::uint64_t) : 0U;
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			const auto offset =
				((instruction >> 10) & 0xfffU) << size_code;
			emit(words, transfer_offset |
				(static_cast<std::uintptr_t>(base_offset) << 16) |
				(static_cast<std::uintptr_t>(offset) << 32) |
				fault_position(context));
		}
	} else if ((instruction & 0x3b00'0000U) == 0x3900'0000U) {
		const auto size_code = instruction >> 30;
		const auto operation = (instruction >> 22) & 3U;
		if (operation >= 2 && size_code != 0) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		} else {
			const unsigned transfer = instruction & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			emit(words, vector_unsigned_memory_gadget(
				size_code, operation, base == 31U));
			const auto transfer_offset = offsetof(CpuState, vectors) +
				transfer * sizeof(VectorRegister);
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			const unsigned scale = operation >= 2U ? 4U : size_code;
			const auto offset =
				((instruction >> 10) & 0xfffU) << scale;
			emit(words, transfer_offset |
				(static_cast<std::uintptr_t>(base_offset) << 16) |
				(static_cast<std::uintptr_t>(offset) << 32));
			emit(words, address);
		}
	} else if ((instruction & 0x3b00'0000U) == 0x3800'0000U &&
		   (instruction & 0x0400'0000U) == 0) {
		const auto size_code = instruction >> 30;
		const auto operation = (instruction >> 22) & 3U;
		const bool prefetch = size_code == 3U && operation == 2U;
		const bool valid_transfer =
			(size_code != 3 || operation <= 1) &&
			(size_code != 2 || operation != 3);
		const bool register_offset =
			(instruction & 0x0020'0000U) != 0;
		const auto option = (instruction >> 13) & 7U;
		const auto mode = (instruction >> 10) & 3U;
		const bool valid_offset = !register_offset ||
			(mode == 2U && (option & 2U) != 0);
		const bool valid_prefetch = prefetch && (register_offset ?
			(mode == 2U && (option & 2U) != 0) : mode == 0U);
		if (valid_prefetch) {
			emit(words, uml_arm64_gadget_nop);
		} else if (prefetch) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		} else if (valid_transfer && valid_offset) {
			const auto *gadgets =
				scalar_other_gadgets(size_code, operation);
			const unsigned transfer = instruction & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			const auto transfer_offset = transfer < 31U ?
				transfer * sizeof(std::uint64_t) : 0U;
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			if (register_offset) {
				const unsigned offset_register =
					(instruction >> 16) & 31U;
				const auto offset_register_offset = offset_register < 31U ?
					offset_register * sizeof(std::uint64_t) :
					offsetof(CpuState, gadget_zero);
				const unsigned extension = option == 2U ? 0U :
					option == 6U ? 2U : 1U;
				const unsigned address_mode = extension +
					((instruction & 0x1000U) != 0 ? 3U : 0U);
				emit(words, gadgets->register_offset[address_mode]
					[base == 31U][transfer == 31U]);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					(static_cast<std::uintptr_t>(
						offset_register_offset) << 32));
			} else {
				const bool overlap = operation != 0U && base != 31U &&
					base == transfer;
				const unsigned address_mode = mode == 1U ?
					(overlap ? 3U : 2U) : mode == 3U ?
					(overlap ? 0U : 1U) : 0U;
				emit(words, gadgets->immediate[address_mode]
					[base == 31U][transfer == 31U]);
				const auto displacement = sign_extend(
					(instruction >> 12) & 0x1ffU, 9);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					(static_cast<std::uint64_t>(displacement) << 32));
			}
			emit(words, address);
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0x3b00'0000U) == 0x3800'0000U) {
		const auto size_code = instruction >> 30;
		const auto operation = (instruction >> 22) & 3U;
		const bool valid_transfer = operation < 2 || size_code == 0;
		const bool register_offset =
			(instruction & 0x0020'0000U) != 0;
		const auto option = (instruction >> 13) & 7U;
		const auto mode = (instruction >> 10) & 3U;
		const bool valid_offset = register_offset ?
			(mode == 2U && (option & 2U) != 0) : mode != 2U;
		if (valid_transfer && valid_offset) {
			const auto *gadgets =
				vector_other_gadgets(size_code, operation);
			const unsigned transfer = instruction & 31U;
			const unsigned base = (instruction >> 5) & 31U;
			const auto transfer_offset = offsetof(CpuState, vectors) +
				transfer * sizeof(VectorRegister);
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			if (register_offset) {
				const unsigned offset_register =
					(instruction >> 16) & 31U;
				const auto offset_register_offset =
					offset_register < 31U ?
					offset_register * sizeof(std::uint64_t) :
					offsetof(CpuState, gadget_zero);
				const unsigned extension = option == 2U ? 0U :
					option == 6U ? 2U : 1U;
				const unsigned address_mode = extension +
					((instruction & 0x1000U) != 0 ? 3U : 0U);
				emit(words, gadgets->register_offset[address_mode]
					[base == 31U]);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					(static_cast<std::uintptr_t>(
						offset_register_offset) << 32));
			} else {
				const unsigned address_mode = mode == 1U ? 2U :
					mode == 3U ? 1U : 0U;
				emit(words, gadgets->immediate[address_mode]
					[base == 31U]);
				const auto displacement = sign_extend(
					(instruction >> 12) & 0x1ffU, 9);
				emit(words, transfer_offset |
					(static_cast<std::uintptr_t>(base_offset) << 16) |
					(static_cast<std::uint64_t>(displacement) << 32));
			}
			emit(words, address);
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0x3a00'0000U) == 0x2800'0000U &&
		   (instruction & 0x0400'0000U) == 0) {
		const auto operation = instruction >> 30;
		const bool load = (instruction & 0x0040'0000U) != 0;
		const auto mode = (instruction >> 23) & 3U;
		const auto first = instruction & 31U;
		const auto second = (instruction >> 10) & 31U;
		const bool valid =
			(operation != 1U || (load && mode != 0U)) &&
			(!load || first != second);
		if (valid) {
			const unsigned base = (instruction >> 5) & 31U;
			const bool overlap = load && base != 31U &&
				(base == first || base == second);
			const unsigned address_mode = mode == 1U ?
				(overlap ? 3U : 2U) : mode == 3U ?
				(overlap ? 0U : 1U) : 0U;
			const unsigned register_mode =
				(first == 31U ? 2U : 0U) +
				(second == 31U ? 1U : 0U);
			const auto *gadgets =
				integer_pair_gadgets(operation, load);
			const auto first_offset = first < 31U ?
				first * sizeof(std::uint64_t) : 0U;
			const auto second_offset = second < 31U ?
				second * sizeof(std::uint64_t) : 0U;
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			const auto displacement = sign_extend(
				(instruction >> 15) & 0x7fU, 7) <<
				(operation >= 2U ? 3U : 2U);
			emit(words, (*gadgets)[address_mode][base == 31U]
				[register_mode]);
			emit(words, first_offset |
				(static_cast<std::uintptr_t>(second_offset) << 9) |
				((static_cast<std::uint64_t>(displacement) & 0x3fffU)
					<< 18) |
				(static_cast<std::uintptr_t>(base_offset) << 32) |
				fault_position(context));
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else if ((instruction & 0x3a00'0000U) == 0x2800'0000U &&
		   (instruction & 0x0400'0000U) != 0) {
		const auto operation = instruction >> 30;
		const bool load = (instruction & 0x0040'0000U) != 0;
		const auto first = instruction & 31U;
		const auto second = (instruction >> 10) & 31U;
		if (!load || first != second) {
			const unsigned base = (instruction >> 5) & 31U;
			const unsigned mode = (instruction >> 23) & 3U;
			const unsigned address_mode = mode == 1U ? 2U :
				mode == 3U ? 1U : 0U;
			const auto &gadgets =
				vector_pair_gadgets(operation, load);
			const auto first_offset = offsetof(CpuState, vectors) +
				first * sizeof(VectorRegister);
			const auto second_offset = offsetof(CpuState, vectors) +
				second * sizeof(VectorRegister);
			const auto base_offset = base < 31U ?
				base * sizeof(std::uint64_t) : offsetof(CpuState, sp);
			const auto displacement = sign_extend(
				(instruction >> 15) & 0x7fU, 7) <<
				(operation < 2U ? operation + 2U : 4U);
			emit(words, gadgets[address_mode][base == 31U]);
			emit(words, first_offset |
				(static_cast<std::uintptr_t>(second_offset) << 16) |
				(static_cast<std::uintptr_t>(base_offset) << 32) |
				((static_cast<std::uint64_t>(displacement) & 0xffffU)
					<< 48));
			emit(words, address);
		} else {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return DecodeResult::terminal;
		}
	} else {
		return implementation_gap(context);
	}
	return DecodeResult::decoded;
}

} // namespace uml::cpu::arm64::gadgets::builder_detail
