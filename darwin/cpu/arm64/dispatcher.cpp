// SPDX-License-Identifier: GPL-2.0-or-later
// Static-gadget dispatch design derived from iSH's arm64 asbestos emulator.
#include "dispatcher.hpp"

#include "detail/bfloat_dot.hpp"
#include "detail/bfloat_matrix.hpp"
#include "detail/bfloat_multiply.hpp"
#include "detail/half_fused_long.hpp"
#include "detail/half_dot.hpp"
#include "detail/half_matrix.hpp"
#include "detail/floating_compare.hpp"
#include "detail/bfloat_conversion.hpp"
#include "detail/floating_complex.hpp"
#include "detail/floating_arithmetic.hpp"
#include "detail/floating_conversion.hpp"
#include "detail/floating_exception.hpp"
#include "detail/floating_fused.hpp"
#include "detail/floating_horizontal.hpp"
#include "detail/floating_integer_conversion.hpp"
#include "detail/floating_reciprocal.hpp"
#include "detail/floating_round.hpp"
#include "detail/floating_scale.hpp"
#include "detail/fp8.hpp"
#include "detail/instruction_helpers.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64 {

namespace {

/* The dispatcher is the pure, debug/reference interpreter. Keep its host
 * floating-point operations under the guest FPCR just as the production
 * gadgets do. Saving and restoring here also makes direct Dispatcher tests
 * independent of the caller's floating-point environment.
 */
class ScopedFloatingEnvironment final {
public:
	ScopedFloatingEnvironment(CpuState &cpu, std::uint32_t instruction) noexcept
		: cpu_{cpu}, active_{
			(instruction & 0x0e00'0000U) == 0x0e00'0000U}
	{
		if (!active_)
			return;
		__asm__ volatile("mrs %0, fpcr" : "=r"(saved_fpcr_) : : "memory");
		__asm__ volatile("mrs %0, fpsr" : "=r"(saved_fpsr_) : : "memory");
		cpu_.access_fp_state();
		const std::uint64_t fpcr = detail::host_fpcr(cpu_.fpcr);
		const std::uint64_t fpsr = 0;
		__asm__ volatile("msr fpcr, %0" : : "r"(fpcr) : "memory");
		__asm__ volatile("msr fpsr, %0" : : "r"(fpsr) : "memory");
	}

	~ScopedFloatingEnvironment()
	{
		if (!active_)
			return;
		std::uint64_t result_fpsr;
		__asm__ volatile("mrs %0, fpsr" : "=r"(result_fpsr) : : "memory");
		cpu_.fpsr |= static_cast<std::uint32_t>(result_fpsr) & 0xf800'009fU;
		__asm__ volatile("msr fpcr, %0" : : "r"(saved_fpcr_) : "memory");
		__asm__ volatile("msr fpsr, %0" : : "r"(saved_fpsr_) : "memory");
	}

	ScopedFloatingEnvironment(const ScopedFloatingEnvironment &) = delete;
	ScopedFloatingEnvironment &operator=(const ScopedFloatingEnvironment &) =
		delete;

private:
	CpuState &cpu_;
	std::uint64_t saved_fpcr_{};
	std::uint64_t saved_fpsr_{};
	bool active_{};
};

} // namespace

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
		return {
			.reason = StopReason::memory_fault,
			.address = cpu.pc,
			.access = ::uml::memory::MemoryAccess::execute,
		};
	const auto instruction = static_cast<std::uint32_t>(
		detail::decode_little_endian(encoded));
	return execute(cpu, memory, instruction);
}

RunResult Dispatcher::execute(CpuState &cpu, GuestMemory &memory,
			      std::uint32_t instruction) const
{
	/* The pure dispatcher is the correctness/debug path. Import its complete
	 * deferred image at every explicit execution boundary so every decoder,
	 * including memory and system-register families, observes one coherent
	 * architectural state. Production engines retain translation-time
	 * selective materialization.
	 */
	cpu.access_fp_state();
	const auto guest_fpcr = cpu.fpcr;
	if (detail::needs_floating_exception_transaction(
		    guest_fpcr, instruction)) {
		const auto original_fpsr = cpu.fpsr;
		CpuState isolated = cpu;
		isolated.fpsr &= ~detail::fpsr_exception_cumulative_mask;

		RunResult result;
		{
			const ScopedFloatingEnvironment floating_environment{
				isolated, instruction};
			result = execute_decoded(isolated, memory, instruction);
		}

		/* A reserved allocation remains illegal even when guest exception
		 * enables are set.  It did not execute and therefore cannot turn
		 * into a floating-point trap.
		 */
		if (result.reason != StopReason::continued)
			return result;

		const auto decision = detail::decide_floating_exception(
			cpu.fpcr, original_fpsr, isolated.fpsr);
		if (decision.traps()) {
			cpu.fpsr = decision.committed_fpsr;
			RunResult trap{};
			trap.reason = StopReason::floating_point_trap;
			trap.address = cpu.pc;
			trap.instruction = instruction;
			trap.syndrome = decision.syndrome;
			return trap;
		}

		/* The isolated execution began with cumulative flags clear. Restore
		 * the flags that predated this instruction before committing it.
		 */
		isolated.fpsr |=
			original_fpsr & detail::fpsr_exception_cumulative_mask;
		cpu = isolated;
		return result;
	}

	const ScopedFloatingEnvironment floating_environment{cpu, instruction};
	return execute_decoded(cpu, memory, instruction);
}

RunResult Dispatcher::execute_decoded(CpuState &cpu, GuestMemory &memory,
				      std::uint32_t instruction) const
{
	/* UDF #imm16 is the permanently undefined A64 instruction class. It is
	 * diagnostically distinct inside the dispatcher from an otherwise valid
	 * instruction that has not been implemented yet, but both remain scoped to
	 * the guest task and become an illegal-instruction exit at the engine
	 * boundary.
	 */
	if ((instruction & 0xffff'0000U) == 0)
		return {StopReason::illegal_instruction, cpu.pc, instruction, 0,
			false};
	if (detail::decode_fp8(instruction).recognized)
		return fp8(cpu, memory, instruction);
	/* These exact scalar/vector allocations overlap several legacy broad SIMD
	 * classes. Resolve and validate the complete family before those decoders. */
	if (detail::decode_floating_horizontal(instruction).recognized)
		return floating_horizontal(cpu, memory, instruction);
	if (detail::decode_bfloat_conversion(instruction).recognized)
		return bfloat_conversion(cpu, memory, instruction);
	if (detail::decode_bfloat_dot(instruction).recognized)
		return bfloat_dot(cpu, memory, instruction);
	if (detail::decode_bfloat_matrix_multiply(instruction))
		return bfloat_matrix_multiply(cpu, memory, instruction);
	if (detail::decode_bfloat_multiply_long(instruction).recognized)
		return bfloat_multiply_long(cpu, memory, instruction);
	if ((instruction & 0xbf20'fc00U) == 0x2e00'fc00U) {
		const unsigned size = (instruction >> 22) & 3U;
		if (size == 0U || size == 2U)
			return {StopReason::illegal_instruction, cpu.pc,
				instruction, 0, false};
	}
	if ((instruction & 0xbf20'fc00U) == 0x2e00'ec00U) {
		const bool q = (instruction & 0x4000'0000U) != 0U;
		const unsigned size = (instruction >> 22) & 3U;
		if ((!q && size == 1U) || (q && size == 3U))
			return {StopReason::illegal_instruction, cpu.pc,
				instruction, 0, false};
	}
	if (detail::decode_half_fused_long(instruction).recognized)
		return half_fused_long(cpu, memory, instruction);
	if (detail::reserved_half_fused_long(instruction))
		return {StopReason::illegal_instruction, cpu.pc, instruction, 0,
			false};
	if (detail::decode_half_dot(instruction).recognized)
		return half_dot(cpu, memory, instruction);
	if (detail::reserved_half_dot_scalar_class(instruction))
		return {StopReason::illegal_instruction, cpu.pc, instruction, 0,
			false};
	if (detail::decode_half_matrix(instruction).recognized)
		return half_matrix(cpu, memory, instruction);
	if (detail::decode_floating_reciprocal(instruction).recognized)
		return floating_reciprocal(cpu, memory, instruction);
	if (detail::decode_floating_complex(instruction).recognized)
		return floating_complex(cpu, memory, instruction);
	if (detail::decode_floating_scale(instruction).recognized)
		return floating_scale(cpu, memory, instruction);
	/* Recognize the complete scalar/vector floating compare allocation before
	 * the overlapping integer SIMD decode classes. The shared strict decoder
	 * rejects reserved shapes before any architectural source is read. */
	if (detail::decode_simd_floating_compare(instruction).recognized)
		return simd_floating_compare(cpu, memory, instruction);
	if (detail::decode_simd_floating_precision_vector(instruction).recognized)
		return simd_floating_precision_vector(cpu, memory, instruction);
	/* FRINT64X/Z vector forms overlap the broad three-same arithmetic class.
	 * Resolve their complete allocation first so reserved FRINT shapes are
	 * rejected by the strict decoder instead of being misexecuted. */
	if (detail::decode_floating_round(instruction).recognized)
		return floating_round(cpu, memory, instruction);
	if (detail::decode_basic_floating_arithmetic(instruction).recognized)
		return floating_basic_arithmetic(cpu, memory, instruction);
	if (detail::decode_fused_floating_arithmetic(instruction).recognized)
		return floating_fused_arithmetic(cpu, memory, instruction);
	if ((instruction & 0xffff'fc00U) == 0x1e7e'0000U)
		return floating_javascript_convert(cpu, memory, instruction);
	if (detail::decode_integer_to_floating(instruction).recognized)
		return integer_to_floating_checked(cpu, memory, instruction);
	if (detail::decode_floating_to_integer(instruction).recognized)
		return floating_to_integer_checked(cpu, memory, instruction);

	static constexpr std::array decoders{
		DecodeEntry{0xffe0'001fU, 0xd400'0001U, &Dispatcher::exception},
		DecodeEntry{0xffe0'001fU, 0xd400'0002U, &Dispatcher::exception},
		DecodeEntry{0xffe0'001fU, 0xd400'0003U, &Dispatcher::exception},
		DecodeEntry{0xffe0'001fU, 0xd420'0000U, &Dispatcher::exception},
		DecodeEntry{0xffe0'001fU, 0xd440'0000U, &Dispatcher::exception},
		DecodeEntry{0xffe0'001fU, 0xd4a0'0001U, &Dispatcher::exception},
		DecodeEntry{0xffe0'001fU, 0xd4a0'0002U, &Dispatcher::exception},
		DecodeEntry{0xffe0'001fU, 0xd4a0'0003U, &Dispatcher::exception},
		DecodeEntry{0xffff'ffffU, 0xd6bf'03e0U, &Dispatcher::exception},
		DecodeEntry{0xffff'ffffU, 0xd69f'03e0U, &Dispatcher::exception},
		DecodeEntry{0xffff'ffffU, 0xd69f'0bffU, &Dispatcher::exception},
		DecodeEntry{0xffff'ffffU, 0xd69f'0fffU, &Dispatcher::exception},
		DecodeEntry{0xffff'ffffU, 0xd503'20ffU,
			&Dispatcher::pointer_auth_strip}, // XPACLRI
		DecodeEntry{0xffff'ffffU, 0xd503'211fU,
			&Dispatcher::pointer_auth_code}, // PACIA1716
		DecodeEntry{0xffff'ffffU, 0xd503'215fU,
			&Dispatcher::pointer_auth_code}, // PACIB1716
		DecodeEntry{0xffff'ffffU, 0xd503'219fU,
			&Dispatcher::pointer_auth_code}, // AUTIA1716
		DecodeEntry{0xffff'ffffU, 0xd503'21dfU,
			&Dispatcher::pointer_auth_code}, // AUTIB1716
		DecodeEntry{0xffff'ffffU, 0xd503'231fU,
			&Dispatcher::pointer_auth_code}, // PACIAZ
		DecodeEntry{0xffff'ffffU, 0xd503'233fU,
			&Dispatcher::pointer_auth_code}, // PACIASP
		DecodeEntry{0xffff'ffffU, 0xd503'235fU,
			&Dispatcher::pointer_auth_code}, // PACIBZ
		DecodeEntry{0xffff'ffffU, 0xd503'237fU,
			&Dispatcher::pointer_auth_code}, // PACIBSP
		DecodeEntry{0xffff'ffffU, 0xd503'239fU,
			&Dispatcher::pointer_auth_code}, // AUTIAZ
		DecodeEntry{0xffff'ffffU, 0xd503'23bfU,
			&Dispatcher::pointer_auth_code}, // AUTIASP
		DecodeEntry{0xffff'ffffU, 0xd503'23dfU,
			&Dispatcher::pointer_auth_code}, // AUTIBZ
		DecodeEntry{0xffff'ffffU, 0xd503'23ffU,
			&Dispatcher::pointer_auth_code}, // AUTIBSP
		/* Architectural HINTs, including NOP and BTI landing pads, have no
		 * guest-visible state in this userspace emulator.
		 */
		DecodeEntry{0xffff'f01fU, 0xd503'201fU, &Dispatcher::nop},
		DecodeEntry{0xffd8'f000U, 0xd500'2000U, &Dispatcher::nop},
		DecodeEntry{0xffff'ffe0U, 0xd503'1000U, &Dispatcher::nop},
		DecodeEntry{0xffff'ffe0U, 0xd503'1020U, &Dispatcher::nop},
		DecodeEntry{0xffff'ffffU, 0xd500'401fU,
			&Dispatcher::flag_manipulation}, // cfinv
		DecodeEntry{0xffff'ffffU, 0xd500'405fU,
			&Dispatcher::flag_manipulation}, // axflag
		DecodeEntry{0xffff'ffffU, 0xd500'403fU,
			&Dispatcher::flag_manipulation}, // xaflag
		DecodeEntry{0xfff8'f01fU, 0xd500'401fU,
			&Dispatcher::pstate_immediate},
		DecodeEntry{0xffff'ffe0U, 0xd53b'd040U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd51b'd040U, &Dispatcher::system_register},
		DecodeEntry{0xffff'ffe0U, 0xd53b'd060U, &Dispatcher::system_register},
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
		/* Decode the complete scalar and pair system-register allocations after
		 * the implemented EL0 registers. The strict handlers diagnose every
		 * inaccessible register instead of falling through as unsupported. */
		DecodeEntry{0xffd0'0000U, 0xd510'0000U,
			&Dispatcher::system_register},
		DecodeEntry{0xffd0'0000U, 0xd550'0000U,
			&Dispatcher::system_register_pair},
		DecodeEntry{0xfff8'0000U, 0xd548'0000U,
			&Dispatcher::system_instruction_pair},
		DecodeEntry{0xffff'ffe0U, 0xd50b'7420U,
			&Dispatcher::cache_maintenance}, // dc zva, Xt
		DecodeEntry{0xffff'ffe0U, 0xd50b'7b20U,
			&Dispatcher::cache_maintenance}, // dc cvau, Xt
		DecodeEntry{0xffff'ffe0U, 0xd50b'7520U,
			&Dispatcher::cache_maintenance}, // ic ivau, Xt
		DecodeEntry{0xffff'ffe0U, 0xd508'77a0U,
			&Dispatcher::guarded_control_stack}, // GCSPOPCX
		DecodeEntry{0xffff'ffe0U, 0xd52b'7720U,
			&Dispatcher::guarded_control_stack}, // GCSPOPM
		DecodeEntry{0xffff'ffe0U, 0xd508'77c0U,
			&Dispatcher::guarded_control_stack}, // GCSPOPX
		DecodeEntry{0xffff'ffe0U, 0xd50b'7700U,
			&Dispatcher::guarded_control_stack}, // GCSPUSHM
		DecodeEntry{0xffff'ffe0U, 0xd508'7780U,
			&Dispatcher::guarded_control_stack}, // GCSPUSHX
		DecodeEntry{0xffff'ffe0U, 0xd50b'7740U,
			&Dispatcher::guarded_control_stack}, // GCSSS1
		DecodeEntry{0xffff'ffe0U, 0xd52b'7760U,
			&Dispatcher::guarded_control_stack}, // GCSSS2
		DecodeEntry{0xfff8'0000U, 0xd508'0000U,
			&Dispatcher::system_instruction}, // SYS
		DecodeEntry{0xfff8'0000U, 0xd528'0000U,
			&Dispatcher::system_instruction}, // SYSL
		DecodeEntry{0xffff'f0ffU, 0xd503'305fU,
			&Dispatcher::clear_exclusive},
		DecodeEntry{0xffff'f0ffU, 0xd503'309fU, &Dispatcher::memory_barrier},
		DecodeEntry{0xffff'f0ffU, 0xd503'30bfU, &Dispatcher::memory_barrier},
		DecodeEntry{0xffff'f0ffU, 0xd503'30dfU, &Dispatcher::memory_barrier},
		DecodeEntry{0xffff'f3ffU, 0xd503'323fU, &Dispatcher::memory_barrier},
		DecodeEntry{0xffff'ffffU, 0xd503'30ffU, &Dispatcher::memory_barrier},
		/* Only CRm==0 allocates SB. Diagnose the rest of this otherwise
		 * unallocated barrier-opcode column as illegal instead of allowing it
		 * to fall through as an unsupported instruction. */
		DecodeEntry{0xffff'f0ffU, 0xd503'30ffU,
			&Dispatcher::unavailable_extension},
		DecodeEntry{0xfedf'f800U, 0xd61f'0800U,
			&Dispatcher::branch_authenticated},
		DecodeEntry{0xffff'fbffU, 0xd65f'0bffU,
			&Dispatcher::branch_authenticated},
		DecodeEntry{0xffff'fbe0U, 0xd65f'0be0U,
			&Dispatcher::unavailable_extension},
		DecodeEntry{0xffc0'0000U, 0x5500'0000U,
			&Dispatcher::unavailable_extension},
		DecodeEntry{0xffff'fc1fU, 0xd61f'0000U, &Dispatcher::branch_register},
		DecodeEntry{0xffff'fc1fU, 0xd63f'0000U, &Dispatcher::branch_register},
		DecodeEntry{0xffff'fc1fU, 0xd65f'0000U, &Dispatcher::branch_register},
		DecodeEntry{0xff00'0000U, 0x5400'0000U, &Dispatcher::branch_conditional},
		DecodeEntry{0x7f00'4000U, 0x7500'0000U,
			&Dispatcher::compare_conditional_branch}, // immediate
		DecodeEntry{0x7f00'c000U, 0x7400'0000U,
			&Dispatcher::compare_conditional_branch}, // W/X registers
		DecodeEntry{0xff00'c000U, 0x7400'8000U,
			&Dispatcher::compare_conditional_branch}, // byte registers
		DecodeEntry{0xff00'c000U, 0x7400'c000U,
			&Dispatcher::compare_conditional_branch}, // halfword registers
		DecodeEntry{0x7e00'0000U, 0x7400'0000U,
			&Dispatcher::compare_conditional_branch}, // reserved CMPBR forms
		DecodeEntry{0x7e00'0000U, 0x3400'0000U, &Dispatcher::compare_branch},
		DecodeEntry{0x7e00'0000U, 0x3600'0000U, &Dispatcher::test_branch},
		DecodeEntry{0x7c00'0000U, 0x1400'0000U, &Dispatcher::branch_immediate},
		DecodeEntry{0x1f00'0000U, 0x1000'0000U, &Dispatcher::pc_relative},
		DecodeEntry{0x1f80'0000U, 0x1280'0000U, &Dispatcher::move_wide},
		DecodeEntry{0x7ff0'0000U, 0x11c0'0000U,
			&Dispatcher::integer_min_max},
		DecodeEntry{0xffc0'c000U, 0x9180'0000U,
			&Dispatcher::mte_add_sub_tag},
		DecodeEntry{0xffc0'c000U, 0xd180'0000U,
			&Dispatcher::mte_add_sub_tag},
		/* The remaining members of the tag-immediate allocation are
		 * unallocated. Keep them out of the broad immediate decoder so a
		 * malformed instruction is diagnosed before any state is observed.
		 */
		DecodeEntry{0x1f80'0000U, 0x1180'0000U,
			&Dispatcher::unavailable_extension},
		DecodeEntry{0x1f80'0000U, 0x1100'0000U, &Dispatcher::add_sub_immediate},
		DecodeEntry{0x1f20'0000U, 0x0b00'0000U, &Dispatcher::add_sub_register},
		DecodeEntry{0x1fe0'0000U, 0x0b20'0000U, &Dispatcher::add_sub_register},
		/* FEAT_MTE pointer arithmetic overlaps the broad add-with-carry
		 * allocation. Decode its fully fixed opcodes first.
		 */
		DecodeEntry{0xffe0'fc00U, 0x9ac0'0000U, &Dispatcher::mte_pointer},
		DecodeEntry{0xffe0'fc00U, 0xbac0'0000U, &Dispatcher::mte_pointer},
		DecodeEntry{0xffe0'fc00U, 0x9ac0'1000U, &Dispatcher::mte_pointer},
		DecodeEntry{0xffe0'fc00U, 0x9ac0'1400U, &Dispatcher::mte_pointer},
		DecodeEntry{0xffe0'e000U, 0x9a00'2000U, &Dispatcher::cpa_pointer},
		DecodeEntry{0xffe0'e000U, 0xda00'2000U, &Dispatcher::cpa_pointer},
		DecodeEntry{0x1fe0'fc00U, 0x1a00'0000U, &Dispatcher::add_sub_carry},
		DecodeEntry{0xffe0'7c10U, 0xba00'0400U,
			&Dispatcher::flag_manipulation}, // rmif
		DecodeEntry{0xffff'bc1fU, 0x3a00'080dU,
			&Dispatcher::flag_manipulation}, // setf8/setf16
		/* Keep reserved members of both flag-manipulation allocations on
		 * the validating interpreter path.  Bit 10 for RMIF and bit 11 for
		 * SETF distinguish neighboring allocated arithmetic forms.
		 */
		DecodeEntry{0xffe0'0400U, 0xba00'0400U,
			&Dispatcher::flag_manipulation},
		DecodeEntry{0xffff'0800U, 0x3a00'0800U,
			&Dispatcher::flag_manipulation},
		DecodeEntry{0x1f80'0000U, 0x1200'0000U, &Dispatcher::logical_immediate},
		DecodeEntry{0x1f80'0000U, 0x1300'0000U, &Dispatcher::bitfield},
		DecodeEntry{0x1f80'0000U, 0x1380'0000U,
			&Dispatcher::extract_register},
		DecodeEntry{0x1f00'0000U, 0x0a00'0000U, &Dispatcher::logical_shifted},
		DecodeEntry{0x3fe0'0000U, 0x1a80'0000U, &Dispatcher::conditional_select},
		DecodeEntry{0x3fe0'0000U, 0x3a40'0000U,
			&Dispatcher::conditional_compare},
		/* FEAT_CPA multiply encodings occupy otherwise reserved members of
		 * the broad data-processing (3 source) allocation.
		 */
		DecodeEntry{0xffe0'8000U, 0x9b60'0000U, &Dispatcher::cpa_multiply},
		DecodeEntry{0xffe0'8000U, 0x9b60'8000U, &Dispatcher::cpa_multiply},
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
		DecodeEntry{0x7fe0'f000U, 0x1ac0'6000U,
			&Dispatcher::integer_min_max},
		/* S==1 members of the scalar min/max opcode partition are
		 * unallocated. Route them through the validating interpreter so
		 * debug execution reports the exact illegal instruction early. */
		DecodeEntry{0x5fe0'f000U, 0x1ac0'6000U,
			&Dispatcher::integer_min_max},
		DecodeEntry{0xffe0'fc00U, 0x9ac0'3000U,
			&Dispatcher::pointer_auth_generic},
		DecodeEntry{0xffff'c000U, 0xdac1'0000U,
			&Dispatcher::pointer_auth_code},
		DecodeEntry{0xffff'f800U, 0xdac1'4000U,
			&Dispatcher::pointer_auth_strip},
		/* FEAT_PAuth_LR is absent from this host-backed guest profile. PACM
		 * retains its HINT-space NOP behavior; every non-HINT encoding is
		 * UNDEFINED before architectural state is observed. */
		DecodeEntry{0xffff'c000U, 0xdac1'8000U,
			&Dispatcher::unavailable_extension},
		DecodeEntry{0xffc0'0000U, 0xf380'0000U,
			&Dispatcher::unavailable_extension},
		DecodeEntry{0x7fff'8000U, 0x5ac0'0000U,
			&Dispatcher::bit_reverse_count},
		DecodeEntry{0x7fe0'e000U, 0x1ac0'4000U, &Dispatcher::crc32},
		/* Every allocated data-processing (2 source) member is decoded
		 * above. Catch its reserved opcode combinations as UNDEFINED before
		 * the diagnostic unsupported-instruction fallback.
		 */
		DecodeEntry{0x5fe0'0000U, 0x1ac0'0000U,
			&Dispatcher::unavailable_extension},
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
		DecodeEntry{0xff3f'fc00U, 0x1e20'4000U,
			&Dispatcher::floating_move},
		DecodeEntry{0xffff'fc00U, 0x1ee6'0000U,
			&Dispatcher::simd_general_move},
		DecodeEntry{0xffff'fc00U, 0x9ee6'0000U,
			&Dispatcher::simd_general_move},
		DecodeEntry{0xffff'fc00U, 0x1ee7'0000U,
			&Dispatcher::simd_general_move},
		DecodeEntry{0xffff'fc00U, 0x9ee7'0000U,
			&Dispatcher::simd_general_move},
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
		DecodeEntry{0xffe0'fc00U, 0xce60'c000U,
			&Dispatcher::simd_crypto}, // sm3partw1
		DecodeEntry{0xffe0'fc00U, 0xce60'c400U,
			&Dispatcher::simd_crypto}, // sm3partw2
		DecodeEntry{0xffe0'8000U, 0xce40'0000U,
			&Dispatcher::simd_crypto}, // sm3ss1
		DecodeEntry{0xffe0'cc00U, 0xce40'8000U,
			&Dispatcher::simd_crypto}, // sm3tt1a
		DecodeEntry{0xffe0'cc00U, 0xce40'8400U,
			&Dispatcher::simd_crypto}, // sm3tt1b
		DecodeEntry{0xffe0'cc00U, 0xce40'8800U,
			&Dispatcher::simd_crypto}, // sm3tt2a
		DecodeEntry{0xffe0'cc00U, 0xce40'8c00U,
			&Dispatcher::simd_crypto}, // sm3tt2b
		DecodeEntry{0xffff'fc00U, 0xcec0'8400U,
			&Dispatcher::simd_crypto}, // sm4e
		DecodeEntry{0xffe0'fc00U, 0xce60'c800U,
			&Dispatcher::simd_crypto}, // sm4ekey
		DecodeEntry{0xffe0'fc00U, 0xce60'cc00U,
			&Dispatcher::simd_crypto}, // unallocated crypto three-register
		DecodeEntry{0xffe0'8000U, 0xce60'0000U,
			&Dispatcher::simd_crypto}, // unallocated crypto four-register
		DecodeEntry{0xffff'fc00U, 0xcec0'8800U,
			&Dispatcher::simd_crypto}, // unallocated crypto two-register
		DecodeEntry{0xffff'fc00U, 0xcec0'8c00U,
			&Dispatcher::simd_crypto}, // unallocated crypto two-register
		DecodeEntry{0xffe0'8000U, 0xce00'0000U,
			&Dispatcher::simd_crypto}, // eor3
		DecodeEntry{0xffe0'fc00U, 0xce60'8c00U,
			&Dispatcher::simd_crypto}, // rax1
		DecodeEntry{0xffe0'0000U, 0xce80'0000U,
			&Dispatcher::simd_crypto}, // xar
		DecodeEntry{0xffe0'8000U, 0xce20'0000U,
			&Dispatcher::simd_crypto}, // bcax
		DecodeEntry{0x9ff8'0400U, 0x0f00'0400U,
			&Dispatcher::simd_modified_immediate},
		DecodeEntry{0x9f20'fc00U, 0x0e00'9400U,
			&Dispatcher::simd_dot_product},
		DecodeEntry{0xbf20'fc00U, 0x0e00'9c00U,
			&Dispatcher::simd_dot_product},
		DecodeEntry{0xdf20'fc00U, 0x5e00'9400U,
			&Dispatcher::simd_dot_product}, // reserved scalar dot slot
		DecodeEntry{0xff20'fc00U, 0x5e00'9c00U,
			&Dispatcher::simd_dot_product}, // reserved scalar mixed slot
		DecodeEntry{0x9f20'f400U, 0x0e00'a400U,
			&Dispatcher::simd_matrix_multiply},
		DecodeEntry{0xbf60'fc00U, 0x0e40'1c00U,
			&Dispatcher::simd_floating_multiply_extended},
		DecodeEntry{0xff60'fc00U, 0x5e40'1c00U,
			&Dispatcher::simd_floating_multiply_extended},
		DecodeEntry{0xbf20'fc00U, 0x0e20'dc00U,
			&Dispatcher::simd_floating_multiply_extended},
		DecodeEntry{0xff20'fc00U, 0x5e20'dc00U,
			&Dispatcher::simd_floating_multiply_extended},
		DecodeEntry{0xbf20'f400U, 0x2e00'8400U,
			&Dispatcher::simd_saturating_rounding_multiply_accumulate},
		DecodeEntry{0xff20'f400U, 0x7e00'8400U,
			&Dispatcher::simd_saturating_rounding_multiply_accumulate},
		/* The broad integer three-same handler owns validation and the pure
		 * reference semantics for every operation in this allocation. Keep it
		 * ahead of operation-specific handlers so malformed size/scalar forms
		 * are rejected before reading registers. Opcode 3 delegates to the
		 * bitwise-register handler. */
		DecodeEntry{0x9f20'0400U, 0x0e20'0400U,
			&Dispatcher::simd_three_same},
		DecodeEntry{0xdf20'0400U, 0x5e20'0400U,
			&Dispatcher::simd_three_same},
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
		DecodeEntry{0xbfbf'fc00U, 0x2ea1'f800U,
			&Dispatcher::simd_floating_unary}, // fsqrt
		DecodeEntry{0xbfff'fc00U, 0x0ef8'f800U,
			&Dispatcher::simd_floating_unary}, // fabs (FP16)
		DecodeEntry{0xbfff'fc00U, 0x2ef8'f800U,
			&Dispatcher::simd_floating_unary}, // fneg (FP16)
		DecodeEntry{0x9f30'0c00U, 0x0e20'0800U,
			&Dispatcher::simd_two_register_misc},
		DecodeEntry{0xdf30'0c00U, 0x5e20'0800U,
			&Dispatcher::simd_two_register_misc},
		DecodeEntry{0x9f30'0c00U, 0x0e30'0800U,
			&Dispatcher::simd_across_lanes},
		DecodeEntry{0xdf30'0c00U, 0x5e30'0800U,
			&Dispatcher::simd_scalar_pairwise},
		/* FEAT_LUT LUTI2/LUTI4. op2=0 remains the pre-existing Q-form
		 * TBL/TBX allocation, so select the three LUT op2 values exactly. */
		DecodeEntry{0xffe0'8c00U, 0x4e40'0000U,
			&Dispatcher::simd_lut_lookup},
		DecodeEntry{0xffe0'8c00U, 0x4e80'0000U,
			&Dispatcher::simd_lut_lookup},
		DecodeEntry{0xffe0'8c00U, 0x4ec0'0000U,
			&Dispatcher::simd_lut_lookup},
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
		DecodeEntry{0x9f80'fc00U, 0x0f00'a400U,
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
		/* FEAT_LRCPC3 AdvSIMD lane acquire/release aliases overlap the broad
		 * single-structure class. Route them first so ordering is not lost. */
		DecodeEntry{0xbfbf'fc00U, 0x0d01'8400U,
			&Dispatcher::load_store_simd_ordered},
		DecodeEntry{0xbf20'0000U, 0x0c00'0000U,
			&Dispatcher::load_store_simd_structure},
		DecodeEntry{0xbf80'0000U, 0x0d00'0000U,
			&Dispatcher::load_store_simd_single_structure},
		DecodeEntry{0xbf80'0000U, 0x0d80'0000U,
			&Dispatcher::load_store_simd_single_structure},
		/* FEAT_LRCPC3 ordered unscaled SIMD&FP transfers. The handler checks
		 * the reserved 128-bit size/opc combinations before observing state. */
		DecodeEntry{0x3f20'0c00U, 0x1d00'0800U,
			&Dispatcher::load_store_simd_ordered},
		DecodeEntry{0xff00'0000U, 0xd800'0000U, &Dispatcher::nop},
		/* Direct FEAT_GCS stores overlap the broad load/store allocation. */
		DecodeEntry{0xffff'fc00U, 0xd91f'0c00U,
			&Dispatcher::guarded_control_stack}, // GCSSTR
		DecodeEntry{0xffff'fc00U, 0xd91f'1c00U,
			&Dispatcher::guarded_control_stack}, // GCSSTTR
		/* FEAT_MTE tag-memory operations overlap the generic pair and
		 * load/store classes. Route their whole allocations to the strict
		 * handler before those broader decoders. */
		DecodeEntry{0xff20'0000U, 0xd920'0000U,
			&Dispatcher::load_store_allocation_tag},
		DecodeEntry{0xffc0'0000U, 0x6880'0000U,
			&Dispatcher::load_store_allocation_tag}, // STGP post-index
		DecodeEntry{0xffc0'0000U, 0x6900'0000U,
			&Dispatcher::load_store_allocation_tag}, // STGP signed offset
		DecodeEntry{0xffc0'0000U, 0x6980'0000U,
			&Dispatcher::load_store_allocation_tag}, // STGP pre-index
		DecodeEntry{0x3f00'0000U, 0x1800'0000U, &Dispatcher::load_literal},
		DecodeEntry{0x3f00'0000U, 0x1c00'0000U, &Dispatcher::load_literal},
		DecodeEntry{0x3a00'0000U, 0x2800'0000U, &Dispatcher::load_store_pair},
		DecodeEntry{0x3fbf'fc00U, 0x1980'0800U,
			&Dispatcher::load_store_ordered_writeback},
		DecodeEntry{0xbfa0'0000U, 0x0980'0000U,
			&Dispatcher::load_store_compare_swap_pair},
		DecodeEntry{0xbfa0'0000U, 0x0820'0000U,
			&Dispatcher::load_store_compare_swap_pair},
		DecodeEntry{0x3fff'fc00U, 0x38bf'c000U,
			&Dispatcher::load_store_rcpc},
		DecodeEntry{0x3f20'0c00U, 0x1900'0000U,
			&Dispatcher::load_store_rcpc},
		/* Route the complete ordered-register-pair allocation to the strict
		 * handler. It validates the individual FEAT_LRCPC3/FEAT_LSCP opcode
		 * before any address or memory access, including reserved opc2 values.
		 */
		DecodeEntry{0x3f20'0c00U, 0x1900'0800U,
			&Dispatcher::load_store_ordered_pair},
		/* FEAT_THE RCW allocations must precede the broader LSE/LSE128
		 * decoders. The strict handlers reject register constraints and
		 * reserved operations before observing an address or memory.
		 */
		DecodeEntry{0xbf20'fc00U, 0x1920'0c00U,
			&Dispatcher::load_store_rcw_pair},
		/* S=1,o3=1,opc=000 is the reserved software-RCW boundary
		 * immediately beside SWPP. Keep it in the strict RCW decoder. */
		DecodeEntry{0xff20'fc00U, 0x5920'8000U,
			&Dispatcher::load_store_rcw_pair},
		DecodeEntry{0xbf20'fc00U, 0x1920'9000U,
			&Dispatcher::load_store_rcw_pair},
		DecodeEntry{0xbf20'fc00U, 0x1920'a000U,
			&Dispatcher::load_store_rcw_pair},
		DecodeEntry{0xbf20'fc00U, 0x1920'b000U,
			&Dispatcher::load_store_rcw_pair},
		DecodeEntry{0xbf20'fc00U, 0x1920'c000U,
			&Dispatcher::load_store_rcw_pair},
		DecodeEntry{0xbf20'fc00U, 0x1920'd000U,
			&Dispatcher::load_store_rcw_pair},
		DecodeEntry{0xbf20'fc00U, 0x1920'e000U,
			&Dispatcher::load_store_rcw_pair},
		DecodeEntry{0xbf20'fc00U, 0x1920'f000U,
			&Dispatcher::load_store_rcw_pair},
		DecodeEntry{0xbf20'fc00U, 0x1920'0800U,
			&Dispatcher::load_store_rcw},
		DecodeEntry{0xbf20'fc00U, 0x3820'9000U,
			&Dispatcher::load_store_rcw},
		DecodeEntry{0xbf20'fc00U, 0x3820'a000U,
			&Dispatcher::load_store_rcw},
		DecodeEntry{0xbf20'fc00U, 0x3820'b000U,
			&Dispatcher::load_store_rcw},
		/* The pair RCW entries above consume every allocated o3=1 cell.
		 * Route the complete remaining LSE128/S=1 boundary through one
		 * strict decoder so reserved software cells diagnose UNDEFINED.
		 */
		DecodeEntry{0xbf20'0c00U, 0x1920'0000U,
			&Dispatcher::load_store_atomic_pair_128},
		DecodeEntry{0xffff'fc00U, 0xf83f'd000U,
			&Dispatcher::load_store_64b},
		DecodeEntry{0xffff'fc00U, 0xf83f'9000U,
			&Dispatcher::load_store_64b},
		DecodeEntry{0xffe0'fc00U, 0xf820'b000U,
			&Dispatcher::load_store_64b},
		DecodeEntry{0xffe0'fc00U, 0xf820'a000U,
			&Dispatcher::load_store_64b},
		/* FEAT_MOPS: ignore sz here so the strict handler diagnoses its
		 * reserved encodings before any register or memory observation. */
		DecodeEntry{0x3fe0'0c00U, 0x1dc0'0400U,
			&Dispatcher::memory_copy_set}, // SETG* (before overlapping CPY*)
		DecodeEntry{0x3f20'0c00U, 0x1900'0400U,
			&Dispatcher::memory_copy_set}, // CPYF*
		DecodeEntry{0x3f20'0c00U, 0x1d00'0400U,
			&Dispatcher::memory_copy_set}, // CPY*
		DecodeEntry{0x3fe0'0c00U, 0x19c0'0400U,
			&Dispatcher::memory_copy_set}, // SET*
		/* Bit 31 is RES0 for FEAT_LSUI scalar atomics. Deliberately leave
		 * it out of the decode mask so the strict handler diagnoses that
		 * reserved state instead of letting it fall into a broad class. */
		DecodeEntry{0x3f20'0c00U, 0x1920'0400U,
			&Dispatcher::load_store_atomic},
		/* FEAT_LSFE uses the scalar-atomic allocation with VR=1. The strict
		 * handler owns reserved opc and malformed store-only encodings too. */
		DecodeEntry{0x3f20'0c00U, 0x3c20'0000U,
			&Dispatcher::load_store_atomic_floating},
		DecodeEntry{0x3f20'0c00U, 0x3820'0000U,
			&Dispatcher::load_store_atomic},
		DecodeEntry{0xbfa0'0000U, 0x8980'0000U,
			&Dispatcher::load_store_compare_swap},
		DecodeEntry{0x3fa0'0000U, 0x08a0'0000U,
			&Dispatcher::load_store_compare_swap},
		DecodeEntry{0xbfa0'0000U, 0x8900'0000U,
			&Dispatcher::load_store_exclusive},
		DecodeEntry{0x3f00'0000U, 0x0800'0000U,
			&Dispatcher::load_store_exclusive},
		DecodeEntry{0xff20'0400U, 0xf820'0400U,
			&Dispatcher::load_pointer_authenticated},
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

} // namespace uml::cpu::arm64
