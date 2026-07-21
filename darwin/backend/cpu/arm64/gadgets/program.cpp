// SPDX-License-Identifier: GPL-2.0-or-later
#include "program.hpp"

#include "../detail/instruction_helpers.hpp"

#include <cstddef>
#include <utility>

namespace uml::backend::arm64::gadgets {
namespace {

extern "C" {
int uml_arm64_gadget_enter(const std::uintptr_t *program, CpuState *cpu,
			    Tlb *tlb);
void uml_arm64_gadget_movz();
void uml_arm64_gadget_movk();
void uml_arm64_gadget_movn();
void uml_arm64_gadget_add_sub_immediate();
void uml_arm64_gadget_add_sub_register();
void uml_arm64_gadget_conditional_select();
void uml_arm64_gadget_logical_immediate();
void uml_arm64_gadget_logical_shifted();
void uml_arm64_gadget_pc_relative();
void uml_arm64_gadget_tls_read();
void uml_arm64_gadget_tls_write();
void uml_arm64_gadget_dup_gpr_to_vector();
void uml_arm64_gadget_decoded();
void uml_arm64_gadget_nop();
void uml_arm64_gadget_load32_unsigned();
void uml_arm64_gadget_load64_unsigned();
void uml_arm64_gadget_store32_unsigned();
void uml_arm64_gadget_store64_unsigned();
void uml_arm64_gadget_load_store_integer_other();
void uml_arm64_gadget_load_store_pair();
void uml_arm64_gadget_svc();
void uml_arm64_gadget_interrupt();
void uml_arm64_gadget_fatal();
void uml_arm64_gadget_branch();
void uml_arm64_gadget_branch_link();
void uml_arm64_gadget_branch_register();
void uml_arm64_gadget_branch_link_register();
void uml_arm64_gadget_return();
void uml_arm64_gadget_branch_eq();
void uml_arm64_gadget_branch_ne();
void uml_arm64_gadget_branch_hs();
void uml_arm64_gadget_branch_lo();
void uml_arm64_gadget_branch_mi();
void uml_arm64_gadget_branch_pl();
void uml_arm64_gadget_branch_vs();
void uml_arm64_gadget_branch_vc();
void uml_arm64_gadget_branch_hi();
void uml_arm64_gadget_branch_ls();
void uml_arm64_gadget_branch_ge();
void uml_arm64_gadget_branch_lt();
void uml_arm64_gadget_branch_gt();
void uml_arm64_gadget_branch_le();
void uml_arm64_gadget_branch_always();
void uml_arm64_gadget_compare_branch();
void uml_arm64_gadget_test_branch();
}

using Gadget = void (*)();

static_assert(offsetof(CpuState, registers) == 0);
static_assert(offsetof(CpuState, sp) == 248);
static_assert(offsetof(CpuState, pc) == 256);
static_assert(offsetof(CpuState, nzcv) == 264);
static_assert(offsetof(CpuState, vectors) == 272);
static_assert(offsetof(CpuState, tls) == 792);

void emit(std::vector<std::uintptr_t> &words, Gadget gadget)
{
	words.push_back(reinterpret_cast<std::uintptr_t>(gadget));
}

void emit(std::vector<std::uintptr_t> &words, std::uintptr_t operand)
{
	words.push_back(operand);
}

[[nodiscard]] Gadget conditional_branch_gadget(unsigned condition) noexcept
{
	switch (condition) {
	case 0: return uml_arm64_gadget_branch_eq;
	case 1: return uml_arm64_gadget_branch_ne;
	case 2: return uml_arm64_gadget_branch_hs;
	case 3: return uml_arm64_gadget_branch_lo;
	case 4: return uml_arm64_gadget_branch_mi;
	case 5: return uml_arm64_gadget_branch_pl;
	case 6: return uml_arm64_gadget_branch_vs;
	case 7: return uml_arm64_gadget_branch_vc;
	case 8: return uml_arm64_gadget_branch_hi;
	case 9: return uml_arm64_gadget_branch_ls;
	case 10: return uml_arm64_gadget_branch_ge;
	case 11: return uml_arm64_gadget_branch_lt;
	case 12: return uml_arm64_gadget_branch_gt;
	case 13: return uml_arm64_gadget_branch_le;
	default: return uml_arm64_gadget_branch_always;
	}
}

[[nodiscard]] std::uintptr_t move_wide_operand(std::uint32_t instruction)
{
	const auto rd = instruction & 31U;
	const auto hw = (instruction >> 21) & 3U;
	const auto immediate = (instruction >> 5) & 0xffffU;
	const auto sf = instruction >> 31;
	return rd | (static_cast<std::uintptr_t>(hw) << 8) |
		(static_cast<std::uintptr_t>(immediate) << 16) |
		(static_cast<std::uintptr_t>(sf) << 32);
}

[[nodiscard]] constexpr std::int64_t sign_extend(std::uint64_t value,
						 unsigned bits) noexcept
{
	const auto shift = 64U - bits;
	return static_cast<std::int64_t>(value << shift) >> shift;
}

} // namespace

Program::Program(std::uint64_t guest_address, std::size_t instruction_count,
		 std::vector<std::uintptr_t> words) noexcept
	: guest_address_{guest_address}, instruction_count_{instruction_count},
	  words_{std::move(words)}
{
}

std::uint64_t Program::guest_address() const noexcept
{
	return guest_address_;
}

std::size_t Program::instruction_count() const noexcept
{
	return instruction_count_;
}

std::span<const std::uintptr_t> Program::words() const noexcept
{
	return words_;
}

BuildResult Builder::build(
	std::uint64_t guest_address,
	std::span<const std::uint32_t> instructions) const
{
	std::vector<std::uintptr_t> words;
	words.reserve(instructions.size() * 2 + 2);
	std::size_t compiled = 0;
	for (const auto instruction : instructions) {
		const auto address = guest_address + compiled * sizeof(instruction);
		if ((instruction & 0x7c00'0000U) == 0x1400'0000U) {
			const auto displacement = sign_extend(
				static_cast<std::uint64_t>(instruction & 0x03ff'ffffU) << 2,
				28);
			const auto target = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + displacement);
			const bool link = (instruction >> 31) != 0;
			emit(words, link ? uml_arm64_gadget_branch_link :
				uml_arm64_gadget_branch);
			emit(words, target);
			if (link)
				emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words)},
				true, address, instruction};
		} else if ((instruction & 0xff00'0010U) == 0x5400'0000U) {
			const auto displacement = sign_extend(
				static_cast<std::uint64_t>((instruction >> 5) & 0x7ffffU) << 2,
				21);
			emit(words, conditional_branch_gadget(instruction & 15U));
			emit(words, static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + displacement));
			emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words)},
				true, address, instruction};
		} else if ((instruction & 0x7e00'0000U) == 0x3400'0000U) {
			const auto displacement = sign_extend(
				static_cast<std::uint64_t>((instruction >> 5) & 0x7ffffU) << 2,
				21);
			emit(words, uml_arm64_gadget_compare_branch);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>(instruction >> 31) << 8) |
				(static_cast<std::uintptr_t>((instruction >> 24) & 1U) << 9));
			emit(words, static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + displacement));
			emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words)},
				true, address, instruction};
		} else if ((instruction & 0x7e00'0000U) == 0x3600'0000U) {
			const auto displacement = sign_extend(
				static_cast<std::uint64_t>((instruction >> 5) & 0x3fffU) << 2,
				16);
			const auto bit = ((instruction >> 31) << 5) |
				((instruction >> 19) & 31U);
			emit(words, uml_arm64_gadget_test_branch);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>(bit) << 8) |
				(static_cast<std::uintptr_t>((instruction >> 24) & 1U) << 16));
			emit(words, static_cast<std::uint64_t>(
				static_cast<std::int64_t>(address) + displacement));
			emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words)},
				true, address, instruction};
		} else if ((instruction & 0xffff'fc1fU) == 0xd61f'0000U ||
			   (instruction & 0xffff'fc1fU) == 0xd63f'0000U ||
			   (instruction & 0xffff'fc1fU) == 0xd65f'0000U) {
			const auto opcode = instruction & 0xffff'fc1fU;
			emit(words, opcode == 0xd61f'0000U ?
				uml_arm64_gadget_branch_register :
				opcode == 0xd63f'0000U ?
				uml_arm64_gadget_branch_link_register :
				uml_arm64_gadget_return);
			emit(words, (instruction >> 5) & 31U);
			if (opcode == 0xd63f'0000U)
				emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words)},
				true, address, instruction};
		} else if ((instruction & 0x1f00'0000U) == 0x1000'0000U) {
			const auto immediate = static_cast<std::uint64_t>(
				((instruction >> 5) & 0x7ffffU) << 2) |
				((instruction >> 29) & 3U);
			const auto relative = sign_extend(immediate, 21);
			const auto value = (instruction >> 31) != 0 ?
				static_cast<std::uint64_t>(
					static_cast<std::int64_t>(address & ~0xfffULL) +
					relative * 4096) :
				static_cast<std::uint64_t>(
					static_cast<std::int64_t>(address) + relative);
			emit(words, uml_arm64_gadget_pc_relative);
			emit(words, instruction & 31U);
			emit(words, value);
		} else if ((instruction & 0xffff'ffe0U) == 0xd53b'd040U ||
			   (instruction & 0xffff'ffe0U) == 0xd51b'd040U) {
			emit(words, (instruction & 0x0020'0000U) != 0 ?
				uml_arm64_gadget_tls_read : uml_arm64_gadget_tls_write);
			emit(words, instruction & 31U);
		} else if ((instruction & 0x7f80'0000U) == 0x1280'0000U) {
			emit(words, uml_arm64_gadget_movn);
			emit(words, move_wide_operand(instruction));
		} else if ((instruction & 0x7f80'0000U) == 0x5280'0000U) {
			emit(words, uml_arm64_gadget_movz);
			emit(words, move_wide_operand(instruction));
		} else if ((instruction & 0x7f80'0000U) == 0x7280'0000U) {
			emit(words, uml_arm64_gadget_movk);
			emit(words, move_wide_operand(instruction));
		} else if ((instruction & 0x1f00'0000U) == 0x1100'0000U) {
			emit(words, uml_arm64_gadget_add_sub_immediate);
			emit(words, instruction);
		} else if ((instruction & 0x1f00'0000U) == 0x0b00'0000U) {
			const bool extended = (instruction & 0x0020'0000U) != 0;
			const unsigned shift = (instruction >> 10) &
				(extended ? 0x7U : 0x3fU);
			const unsigned shift_type = (instruction >> 22) & 0x3U;
			const bool is_64_bit = (instruction >> 31) != 0;
			if ((extended && shift > 4) ||
			    (!extended && (shift_type == 3 ||
				(!is_64_bit && shift >= 32)))) {
				emit(words, uml_arm64_gadget_fatal);
				emit(words, address);
				return {Program{guest_address, compiled, std::move(words)},
					false, address, instruction};
			}
			emit(words, uml_arm64_gadget_add_sub_register);
			emit(words, instruction);
		} else if ((instruction & 0x1f00'0000U) == 0x0a00'0000U) {
			const bool is_64_bit = (instruction >> 31) != 0;
			const unsigned shift = (instruction >> 10) & 0x3fU;
			if (!is_64_bit && shift >= 32) {
				emit(words, uml_arm64_gadget_fatal);
				emit(words, address);
				return {Program{guest_address, compiled, std::move(words)},
					false, address, instruction};
			}
			emit(words, uml_arm64_gadget_logical_shifted);
			emit(words, instruction);
		} else if ((instruction & 0x3fe0'0000U) == 0x1a80'0000U) {
			emit(words, uml_arm64_gadget_conditional_select);
			emit(words, instruction);
		} else if ((instruction & 0x1f80'0000U) == 0x1200'0000U) {
			const bool is_64_bit = (instruction >> 31) != 0;
			const auto immediate = detail::decode_logical_immediate(
				is_64_bit, (instruction >> 22) & 1U,
				(instruction >> 16) & 0x3fU,
				(instruction >> 10) & 0x3fU);
			if (!immediate) {
				emit(words, uml_arm64_gadget_fatal);
				emit(words, address);
				return {Program{guest_address, compiled, std::move(words)},
					false, address, instruction};
			}
			emit(words, uml_arm64_gadget_logical_immediate);
			emit(words, instruction);
			emit(words, *immediate);
		} else if (instruction == 0xd503'201fU ||
			   instruction == 0xd503'245fU) {
			emit(words, uml_arm64_gadget_nop);
		} else if ((instruction & 0xbfe0'fc00U) == 0x0e00'0c00U) {
			const auto immediate = (instruction >> 16) & 31U;
			const auto element_size = immediate & 1U ? 0U :
				immediate & 2U ? 1U : immediate & 4U ? 2U :
				immediate & 8U ? 3U : 4U;
			if (element_size == 4U) {
				emit(words, uml_arm64_gadget_fatal);
				emit(words, address);
				return {Program{guest_address, compiled, std::move(words)},
					false, address, instruction};
			}
			emit(words, uml_arm64_gadget_dup_gpr_to_vector);
			emit(words, (instruction & 31U) |
				(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
				(static_cast<std::uintptr_t>(element_size) << 16) |
				(static_cast<std::uintptr_t>((instruction >> 30) & 1U) << 24));
		} else if ((instruction & 0xffc0'0000U) == 0xb940'0000U ||
			   (instruction & 0xffc0'0000U) == 0xf940'0000U ||
			   (instruction & 0xffc0'0000U) == 0xb900'0000U ||
			   (instruction & 0xffc0'0000U) == 0xf900'0000U) {
			const auto opcode = instruction & 0xffc0'0000U;
			const auto gadget = opcode == 0xb940'0000U ?
				uml_arm64_gadget_load32_unsigned :
				opcode == 0xf940'0000U ?
				uml_arm64_gadget_load64_unsigned :
				opcode == 0xb900'0000U ?
				uml_arm64_gadget_store32_unsigned :
				uml_arm64_gadget_store64_unsigned;
			emit(words, gadget);
			const auto reg = instruction & 31U;
			const auto base = (instruction >> 5) & 31U;
			const auto scale = opcode == 0xb940'0000U ||
				opcode == 0xb900'0000U ? 2U : 3U;
			const auto offset = ((instruction >> 10) & 0xfffU) << scale;
			emit(words, reg | (static_cast<std::uintptr_t>(base) << 8) |
				(static_cast<std::uintptr_t>(offset) << 16));
			/* The fast gadget branches to the generic static decoder when
			 * the runtime address crosses a guest page. Keep the decoder's
			 * instruction/PC pair adjacent in the threaded stream.
			 */
			emit(words, instruction);
			emit(words, address);
		} else if ((instruction & 0x3b00'0000U) == 0x3800'0000U &&
			   (instruction & 0x0400'0000U) == 0) {
			const auto size_code = instruction >> 30;
			const auto operation = (instruction >> 22) & 3U;
			const bool valid_transfer =
				(size_code != 3 || operation <= 1) &&
				(size_code != 2 || operation != 3);
			const bool register_offset =
				(instruction & 0x0020'0000U) != 0;
			const auto option = (instruction >> 13) & 7U;
			const bool valid_offset = !register_offset || option == 2 ||
				option == 3 || option == 6 || option == 7;
			if (valid_transfer && valid_offset) {
				emit(words, uml_arm64_gadget_load_store_integer_other);
				emit(words, instruction);
				emit(words, address);
			} else {
				emit(words, uml_arm64_gadget_decoded);
				emit(words, instruction);
				emit(words, address);
			}
		} else if ((instruction & 0x3a00'0000U) == 0x2800'0000U &&
			   (instruction & 0x0400'0000U) == 0 &&
			   ((instruction >> 30) == 0 || (instruction >> 30) == 2)) {
			emit(words, uml_arm64_gadget_load_store_pair);
			emit(words, instruction);
			emit(words, address);
		} else if ((instruction & 0xffe0'001fU) == 0xd400'0001U) {
			emit(words, uml_arm64_gadget_svc);
			emit(words, address + sizeof(instruction));
			++compiled;
			return {Program{guest_address, compiled, std::move(words)},
				true, address, instruction};
		} else {
			emit(words, uml_arm64_gadget_decoded);
			emit(words, instruction);
			emit(words, address);
		}
		++compiled;
	}
	emit(words, uml_arm64_gadget_interrupt);
	emit(words, guest_address + compiled * sizeof(std::uint32_t));
	return {Program{guest_address, compiled, std::move(words)}, true,
		guest_address + compiled * sizeof(std::uint32_t), 0};
}

Event execute(const Program &program, CpuState &cpu, Tlb &tlb) noexcept
{
	if (program.words().empty())
		return Event::fatal;
	return static_cast<Event>(
		uml_arm64_gadget_enter(program.words().data(), &cpu, &tlb));
}

} // namespace uml::backend::arm64::gadgets
