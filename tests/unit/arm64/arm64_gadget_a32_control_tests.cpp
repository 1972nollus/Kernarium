// SPDX-License-Identifier: GPL-2.0-or-later
/* The gadget engine's AArch32 control contract: what an A32 or T32 entry
 * retires, where it stops and what it leaves in the state at each exit
 * (aarch32/guest_key.hpp, engine_execution.hpp). Instruction semantics are
 * the corpus's; these programs pin the translator's control behaviour:
 *
 * - retirement counts, with skipped conditional instructions retired and a
 *   faulting or undefined instruction not;
 * - the key every exit leaves in cpu.pc, the architectural fault address,
 *   and ITSTATE at SVC, at a fault and in an entry in the middle of a block;
 * - interworking branches in both directions and the A32 target with
 *   bit 1 set;
 * - the instruction budget, single step and code that changes in place;
 * - exclusives, the CP15 and counter reads, the system forms; and
 * - LDM, STM and LDRD across a page boundary, mapped and not.
 *
 * Everything runs the production loop (run_translated_aarch32_blocks)
 * over the gadget executor, in whichever guest page size the build has. */
#include "arm64_gadget_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace ::uml::emulator::tests::a32;

/* The encodings the programs use, by instruction. */
namespace a {
constexpr std::uint32_t mov_r0_1 = 0xe3a0'0001U;
constexpr std::uint32_t mov_r1_2 = 0xe3a0'1002U;
constexpr std::uint32_t mov_r1_7 = 0xe3a0'1007U;
constexpr std::uint32_t add_r0_r0_2 = 0xe280'0002U;
constexpr std::uint32_t svc = 0xef00'0000U;
constexpr std::uint32_t cmp_r0_0 = 0xe350'0000U;
constexpr std::uint32_t addne_r1_r1_1 = 0x1281'1001U;
constexpr std::uint32_t addeq_r2_r2_1 = 0x0282'2001U;
constexpr std::uint32_t bne_plus_0 = 0x1a00'0000U; /* to the instruction after the next */
constexpr std::uint32_t b_self = 0xeaff'fffeU;
constexpr std::uint32_t ldr_r1_r2 = 0xe592'1000U;
constexpr std::uint32_t udf = 0xe7f0'00f0U;
constexpr std::uint32_t bkpt_1234 = 0xe121'2374U;
constexpr std::uint32_t bx_r0 = 0xe12f'ff10U;
constexpr std::uint32_t blx_r1 = 0xe12f'ff31U;
constexpr std::uint32_t ldrex_r1_r2 = 0xe192'1f9fU;
constexpr std::uint32_t strex_r3_r1_r2 = 0xe182'3f91U;
constexpr std::uint32_t clrex = 0xf57f'f01fU;
constexpr std::uint32_t cmp_r3_0 = 0xe353'0000U;
constexpr std::uint32_t mrc_tpidruro = 0xee1d'0f70U;
constexpr std::uint32_t mrc_tpidrurw = 0xee1d'0f50U;
constexpr std::uint32_t mcr_tpidrurw = 0xee0d'0f50U;
constexpr std::uint32_t mcr_tpidruro = 0xee0d'0f70U;
constexpr std::uint32_t mcr_dmb = 0xee07'0fbaU;
constexpr std::uint32_t mrc_cntfrq = 0xee1e'0f10U;
constexpr std::uint32_t mrrc_cntvct = 0xec51'0f1eU;
constexpr std::uint32_t mrs_apsr = 0xe10f'0000U;
constexpr std::uint32_t mov_r0_pc = 0xe1a0'000fU;
constexpr std::uint32_t ldm_r0_r1_r4 = 0xe890'001eU;
constexpr std::uint32_t stm_r0_wb_r1_r4 = 0xe8a0'001eU;
constexpr std::uint32_t ldrd_r2_r0 = 0xe1c0'20d0U;
} // namespace a

namespace t {
constexpr std::uint16_t movs_r0_1 = 0x2001;
constexpr std::uint16_t movs_r0_2 = 0x2002;
constexpr std::uint16_t movs_r1_7 = 0x2107;
constexpr std::uint16_t cmp_r0_0 = 0x2800;
constexpr std::uint16_t cmp_r3_0 = 0x2b00;
constexpr std::uint16_t svc = 0xdf00;
constexpr std::uint16_t it_eq = 0xbf08;
constexpr std::uint16_t itt_eq = 0xbf04;
constexpr std::uint16_t ldr_r1_r2 = 0x6811;
constexpr std::uint16_t str_r1_r2 = 0x6011;
constexpr std::uint16_t udf = 0xde00;
constexpr std::uint16_t bkpt_42 = 0xbe42;
constexpr std::uint16_t bx_r0 = 0x4700;
constexpr std::uint16_t mov_r0_pc = 0x4678;
constexpr std::uint16_t b_self = 0xe7fe;
} // namespace t

/* ------------------------------------------------------------ retirement */

void straight_line_retires_every_instruction()
{
	{
		Guest guest{930};
		guest.a32(0, {a::mov_r0_1, a::add_r0_r0_2, a::svc});
		auto cpu = a32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == 3);
		require(cpu.pc == aarch32::a32_key(code_base + 12));
		require(reg(cpu, 0) == 3);
		require(cpu.a32_itstate == 0);
	}
	{
		/* One 16-bit, one 32-bit, one 16-bit: three instructions in
		 * eight bytes. */
		Guest guest{931};
		guest.t32(0, {t::movs_r0_1, 0xf100, 0x0002, t::svc});
		auto cpu = t32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == 3);
		require(cpu.pc == aarch32::t32_key(code_base + 8));
		require(reg(cpu, 0) == 3);
	}
}

void skipped_conditional_instructions_retire()
{
	Guest guest{932};
	guest.a32(0, {0xe3a0'0000U, a::cmp_r0_0, a::addne_r1_r1_1,
		      a::addeq_r2_r2_1, a::svc});
	auto cpu = a32_state();
	cpu.registers[1] = 10;
	cpu.registers[2] = 20;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::system_call);
	require(result.retired_instructions == 5);
	require(reg(cpu, 1) == 10 && reg(cpu, 2) == 21);
}

void conditional_branch_retires_taken_or_not()
{
	for (const bool taken : {false, true}) {
		Guest guest{taken ? 933UL : 934UL};
		/* cmp r0,#0 ; bne over ; mov r1,#7 ; over: svc */
		guest.a32(0, {a::cmp_r0_0, a::bne_plus_0, a::mov_r1_7, a::svc});
		auto cpu = a32_state();
		cpu.registers[0] = taken ? 1 : 0;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == (taken ? 3U : 4U));
		require(reg(cpu, 1) == (taken ? 0U : 7U));
		require(cpu.pc == aarch32::a32_key(code_base + 16));
	}
}

void conditional_supervisor_call_is_skipped_or_taken()
{
	for (const bool taken : {false, true}) {
		Guest guest{taken ? 935UL : 936UL};
		/* cmp r0,#0 ; svceq ; mov r1,#7 ; svc */
		guest.a32(0, {a::cmp_r0_0, 0x0f00'0000U, a::mov_r1_7, a::svc});
		auto cpu = a32_state();
		cpu.registers[0] = taken ? 0 : 1;
		auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		if (taken) {
			require(result.retired_instructions == 2);
			require(cpu.pc == aarch32::a32_key(code_base + 8));
			require(reg(cpu, 1) == 0);
		} else {
			require(result.retired_instructions == 4);
			require(cpu.pc == aarch32::a32_key(code_base + 16));
			require(reg(cpu, 1) == 7);
		}
	}
}

/* ------------------------------------------------------------ exits */

void a_data_fault_stops_at_the_instruction()
{
	{
		Guest guest{937};
		guest.a32(0, {a::mov_r0_1, a::ldr_r1_r2, a::svc});
		auto cpu = a32_state();
		cpu.registers[1] = 0x1111;
		cpu.registers[2] = unmapped + 0x40;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::memory_fault);
		require(result.retired_instructions == 1);
		require(cpu.pc == aarch32::a32_key(code_base + 4));
		require(result.fault.address == unmapped + 0x40);
		require(result.fault.access == ::uml::memory::MemoryAccess::read);
		require(reg(cpu, 0) == 1 && reg(cpu, 1) == 0x1111);
	}
	{
		Guest guest{938};
		guest.t32(0, {t::movs_r0_1, t::str_r1_r2, t::svc});
		auto cpu = t32_state();
		cpu.registers[2] = unmapped + 0x80;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::memory_fault);
		require(result.retired_instructions == 1);
		require(cpu.pc == aarch32::t32_key(code_base + 2));
		require(result.fault.address == unmapped + 0x80);
		require(result.fault.access == ::uml::memory::MemoryAccess::write);
		require(cpu.a32_itstate == 0);
	}
}

void a_fault_inside_an_it_block_keeps_its_itstate()
{
	/* cmp r0,#0 ; it eq ; ldreq r1,[r2] ; svc */
	Guest guest{939};
	guest.t32(0, {t::cmp_r0_0, t::it_eq, t::ldr_r1_r2, t::svc});
	auto cpu = t32_state();
	cpu.registers[2] = unmapped;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::memory_fault);
	require(result.retired_instructions == 2);
	require(cpu.pc == aarch32::t32_key(code_base + 4));
	require(cpu.a32_itstate == 0x08);
	require(result.fault.address == unmapped);

	/* The same block with the condition false does not touch memory. */
	Guest other{940};
	other.t32(0, {t::cmp_r0_0, t::it_eq, t::ldr_r1_r2, t::svc});
	auto skipped = t32_state();
	skipped.registers[0] = 1;
	skipped.registers[2] = unmapped;
	const auto done = other.run(skipped);
	require(done.reason == ExitReason::system_call);
	require(done.retired_instructions == 4);
	require(skipped.a32_itstate == 0);
}

void a_supervisor_call_inside_an_it_block_resumes_mid_block()
{
	/* cmp r0,#0 ; itt eq ; svceq ; moveq r0,#2 ; svc */
	Guest guest{941};
	guest.t32(0, {t::cmp_r0_0, t::itt_eq, t::svc, t::movs_r0_2, t::svc});
	auto cpu = t32_state();
	auto result = guest.run(cpu);
	require(result.reason == ExitReason::system_call);
	require(result.retired_instructions == 3);
	require(cpu.pc == aarch32::t32_key(code_base + 6));
	/* ITSTATE for the next instruction: ITAdvance of 0x04. */
	require(cpu.a32_itstate == 0x08);
	require(reg(cpu, 0) == 0);

	/* The entry in the middle of the block runs one instruction at a time
	 * until ITSTATE is zero, then the rest as usual. */
	result = guest.run(cpu);
	require(result.reason == ExitReason::system_call);
	require(result.retired_instructions == 2);
	require(cpu.pc == aarch32::t32_key(code_base + 10));
	require(cpu.a32_itstate == 0);
	require(reg(cpu, 0) == 2);

	/* A condition that fails skips the rest of the block's instructions
	 * from the same entry. */
	Guest failing{942};
	failing.t32(0, {t::cmp_r0_0, t::itt_eq, t::svc, t::movs_r0_2, t::svc});
	auto skipped = t32_state();
	skipped.registers[0] = 5;
	require(failing.run(skipped).retired_instructions == 5);
	require(reg(skipped, 0) == 5);
}

void an_undefined_instruction_does_not_retire()
{
	{
		Guest guest{943};
		guest.a32(0, {a::mov_r0_1, a::udf, a::svc});
		auto cpu = a32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::illegal_instruction);
		require(result.retired_instructions == 1);
		require(cpu.pc == aarch32::a32_key(code_base + 4));
	}
	{
		Guest guest{944};
		guest.t32(0, {t::movs_r0_1, t::udf, t::svc});
		auto cpu = t32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::illegal_instruction);
		require(result.retired_instructions == 1);
		require(cpu.pc == aarch32::t32_key(code_base + 2));
	}
	/* A word the profile does not have: SWP is undefined, never run. */
	Guest swp{945};
	swp.a32(0, {a::mov_r0_1, 0xe102'1093U, a::svc});
	auto cpu = a32_state();
	const auto result = swp.run(cpu);
	require(result.reason == ExitReason::illegal_instruction);
	require(result.retired_instructions == 1);
	require(cpu.pc == aarch32::a32_key(code_base + 4));
}

void a_conditional_undefined_word_is_skipped_when_its_condition_fails()
{
	/* An unallocated coprocessor word under a condition that fails
	 * retires like any skipped instruction; one that passes is the
	 * undefined-instruction exit. mrc p10 (VFP) is not in the base
	 * profile. */
	const std::uint32_t vmov_s0_r0 = 0x0e00'0a10U; /* vmoveq s0, r0 */
	for (const bool pass : {false, true}) {
		Guest guest{pass ? 946UL : 947UL};
		guest.a32(0, {a::cmp_r0_0, vmov_s0_r0, a::svc});
		auto cpu = a32_state();
		cpu.registers[0] = pass ? 0 : 1;
		const auto result = guest.run(cpu);
		if (pass) {
			require(result.reason == ExitReason::illegal_instruction);
			require(result.retired_instructions == 1);
			require(cpu.pc == aarch32::a32_key(code_base + 4));
		} else {
			require(result.reason == ExitReason::system_call);
			require(result.retired_instructions == 3);
		}
	}
}

/* BKPT retires like SVC and the A64 BRK do, and leaves the PC at itself. */
void a_breakpoint_reports_its_immediate()
{
	{
		Guest guest{948};
		guest.a32(0, {a::mov_r0_1, a::bkpt_1234, a::svc});
		auto cpu = a32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::breakpoint);
		require(result.retired_instructions == 2);
		require(result.syndrome == 0x1234);
		require(cpu.pc == aarch32::a32_key(code_base + 4));
	}
	{
		Guest guest{949};
		guest.t32(0, {t::movs_r0_1, t::bkpt_42, t::svc});
		auto cpu = t32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::breakpoint);
		require(result.retired_instructions == 2);
		require(result.syndrome == 0x42);
		require(cpu.pc == aarch32::t32_key(code_base + 2));
	}
}

/* ------------------------------------------------------------ branches */

void interworking_branches_switch_state_both_ways()
{
	{
		/* A32 bx to a T32 address (bit 0 set). */
		Guest guest{950};
		guest.a32(0, {a::bx_r0});
		guest.t32(0x100, {t::movs_r1_7, t::svc});
		auto cpu = a32_state();
		cpu.registers[0] = code_base + 0x100 + 1;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == 3);
		require(cpu.pc == aarch32::t32_key(code_base + 0x100 + 4));
		require(reg(cpu, 1) == 7);
	}
	{
		/* T32 bx to an A32 address (bit 0 clear). */
		Guest guest{951};
		guest.t32(0, {t::bx_r0});
		guest.a32(0x100, {a::mov_r1_7, a::svc});
		auto cpu = t32_state();
		cpu.registers[0] = code_base + 0x100;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == 3);
		require(cpu.pc == aarch32::a32_key(code_base + 0x100 + 8));
		require(reg(cpu, 1) == 7);
	}
	{
		/* blx r1 from A32 to T32 sets lr to the next A32 instruction. */
		Guest guest{952};
		guest.a32(0, {a::blx_r1});
		guest.t32(0x100, {t::movs_r1_7, t::svc});
		auto cpu = a32_state();
		cpu.registers[1] = code_base + 0x100 + 1;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(reg(cpu, 14) == code_base + 4);
	}
}

void an_a32_target_with_bit_one_set_is_a_pc_alignment_exit()
{
	Guest guest{953};
	guest.a32(0, {a::mov_r1_2, a::bx_r0});
	auto cpu = a32_state();
	cpu.registers[0] = code_base + 0x102;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::pc_alignment);
	require(result.retired_instructions == 2);
	require(result.fault.address == code_base + 0x102);
	require(result.fault.access == ::uml::memory::MemoryAccess::execute);
}

void a_fetch_fault_reports_the_unfetchable_address()
{
	Guest guest{954};
	guest.a32(0, {a::mov_r1_2, a::bx_r0});
	auto cpu = a32_state();
	cpu.registers[0] = unmapped;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::memory_fault);
	require(result.retired_instructions == 2);
	require(result.fault.address == unmapped);
	require(result.fault.access == ::uml::memory::MemoryAccess::execute);
	require(cpu.pc == aarch32::a32_key(unmapped));
}

void the_pc_reads_as_the_architectural_value()
{
	{
		Guest guest{955};
		guest.a32(0, {a::mov_r1_2, a::mov_r0_pc, a::svc});
		auto cpu = a32_state();
		(void)guest.run(cpu);
		require(reg(cpu, 0) == code_base + 4 + 8);
	}
	{
		Guest guest{956};
		guest.t32(0, {t::movs_r0_1, t::mov_r0_pc, t::svc});
		auto cpu = t32_state();
		(void)guest.run(cpu);
		require(reg(cpu, 0) == code_base + 2 + 4);
	}
}

/* ------------------------------------------------------------ loop control */

void the_instruction_budget_ends_a_loop()
{
	for (const bool thumb : {false, true}) {
		Guest guest{thumb ? 957UL : 958UL};
		if (thumb)
			guest.t32(0, {t::b_self});
		else
			guest.a32(0, {a::b_self});
		auto cpu = thumb ? t32_state() : a32_state();
		const auto entry = cpu.pc;
		const auto result = guest.run(cpu, 100);
		require(result.reason == ExitReason::instruction_budget);
		require(result.retired_instructions >= 100);
		require(cpu.pc == entry);
	}
}

void single_step_retires_one_instruction_at_a_time()
{
	for (const bool thumb : {false, true}) {
		Guest guest{thumb ? 959UL : 960UL};
		if (thumb)
			guest.t32(0, {t::movs_r0_1, 0xf100, 0x0002, t::svc});
		else
			guest.a32(0, {a::mov_r0_1, a::add_r0_r0_2, a::svc});
		auto cpu = thumb ? t32_state() : a32_state();
		const std::uint32_t second = thumb ? 2U : 4U;
		const std::uint32_t third = thumb ? 6U : 8U;
		auto step = guest.run(cpu, 100, true);
		require(step.reason == ExitReason::single_step);
		require(step.retired_instructions == 1);
		require(cpu.pc == (thumb ? aarch32::t32_key(code_base + second)
					 : aarch32::a32_key(code_base + second)));
		require(reg(cpu, 0) == 1);
		step = guest.run(cpu, 100, true);
		require(step.reason == ExitReason::single_step);
		require(step.retired_instructions == 1);
		require(cpu.pc == (thumb ? aarch32::t32_key(code_base + third)
					 : aarch32::a32_key(code_base + third)));
		require(reg(cpu, 0) == 3);
		step = guest.run(cpu, 100, true);
		require(step.reason == ExitReason::system_call);
		require(step.retired_instructions == 1);
	}
}

void single_step_walks_an_it_block()
{
	/* cmp r0,#0 ; itt eq ; moveq r0,#2 ; addeq... ; svc */
	Guest guest{961};
	guest.t32(0, {t::cmp_r0_0, t::itt_eq, t::movs_r0_2, t::movs_r1_7, t::svc});
	auto cpu = t32_state();
	std::vector<std::uint8_t> itstates;
	for (int step = 0; step < 4; ++step) {
		const auto result = guest.run(cpu, 100, true);
		require(result.reason == ExitReason::single_step);
		require(result.retired_instructions == 1);
		itstates.push_back(cpu.a32_itstate);
	}
	/* After cmp: 0; after itt eq: the block's ITSTATE; after the first
	 * conditional instruction: advanced; after the last: 0. */
	require(itstates == (std::vector<std::uint8_t>{0x00, 0x04, 0x08, 0x00}));
	require(reg(cpu, 0) == 2 && reg(cpu, 1) == 7);
}

void changed_code_is_translated_again()
{
	{
		Guest guest{962};
		guest.a32(0, {a::mov_r0_1, a::svc});
		auto cpu = a32_state();
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(reg(cpu, 0) == 1);
		const std::uint32_t replacement = 0xe3a0'0009U; /* mov r0,#9 */
		guest.patch(code_base, std::as_bytes(std::span{&replacement, 1}));
		cpu = a32_state();
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(reg(cpu, 0) == 9);
	}
	{
		/* A halfword of a 32-bit T32 instruction. */
		Guest guest{963};
		guest.t32(0, {0xf04f, 0x0001, t::svc}); /* mov.w r0,#1 */
		auto cpu = t32_state();
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(reg(cpu, 0) == 1);
		const std::uint16_t replacement = 0x0005;
		guest.patch(code_base + 2, std::as_bytes(std::span{&replacement, 1}));
		cpu = t32_state();
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(reg(cpu, 0) == 5);
	}
}

void a_a32_block_and_a_t32_block_at_one_address_are_distinct()
{
	/* Translations of the two instruction sets coexist in one cache: the
	 * key, which carries the set, names the translation. */
	Guest guest{964};
	guest.a32(0, {a::mov_r0_1, a::svc});
	auto first = a32_state();
	require(guest.run(first).reason == ExitReason::system_call);
	require(reg(first, 0) == 1);
	guest.t32(0x40, {t::movs_r0_2, t::svc});
	auto second = t32_state(code_base + 0x40);
	require(guest.run(second).reason == ExitReason::system_call);
	require(reg(second, 0) == 2);
	first = a32_state();
	require(guest.run(first).reason == ExitReason::system_call);
	require(reg(first, 0) == 1);
}

/* ------------------------------------------------------------ system */

/* A host store-exclusive may fail spuriously, and the first pass over a
 * page the host has not touched does, so a success is always a loop:
 *   top: ldrex r1,[r2] ; add r1,r1,#1 ; strex r3,r1,[r2] ; cmp r3,#0 ;
 *        bne top ; svc */
void exclusive_loops_complete_and_store_once()
{
	for (const bool thumb : {false, true}) {
		Guest guest{thumb ? 965UL : 966UL};
		if (thumb)
			guest.t32(0, {0xe852, 0x1f00, 0x1c49, 0xe842, 0x1300,
				      t::cmp_r3_0, 0xd1f8 /* bne top */, t::svc});
		else
			guest.a32(0, {a::ldrex_r1_r2, 0xe281'1001U,
				      a::strex_r3_r1_r2, a::cmp_r3_0,
				      0x1aff'fffaU /* bne top */, a::svc});
		guest.data32(0x20, 41);
		auto cpu = thumb ? t32_state() : a32_state();
		cpu.registers[2] = data_base + 0x20;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(reg(cpu, 3) == 0);
		require(guest.data32(0x20) == 42);
	}
}

void a_store_without_a_reservation_fails()
{
	/* No load, a cleared reservation, and a load of another address:
	 * none may store. */
	Guest lone{967};
	lone.a32(0, {a::strex_r3_r1_r2, a::svc});
	lone.data32(0x20, 41);
	auto unreserved = a32_state();
	unreserved.registers[1] = 99;
	unreserved.registers[2] = data_base + 0x20;
	require(lone.run(unreserved).reason == ExitReason::system_call);
	require(reg(unreserved, 3) == 1);
	require(lone.data32(0x20) == 41);

	Guest cleared{968};
	cleared.a32(0, {a::ldrex_r1_r2, a::clrex, a::strex_r3_r1_r2, a::svc});
	cleared.data32(0x20, 41);
	auto after_clear = a32_state();
	after_clear.registers[2] = data_base + 0x20;
	require(cleared.run(after_clear).reason == ExitReason::system_call);
	require(reg(after_clear, 3) == 1);
	require(cleared.data32(0x20) == 41);
}

/* The mechanism the exclusives share with A64 (engine/exclusive.inc) makes
 * a loop finish whatever the TLB holds: plain stores to pages that share
 * the target's TLB set, before or between the exclusive load and store,
 * would otherwise evict the target's write entry on every pass. */
[[nodiscard]] constexpr std::uint64_t tlb_set(std::uint64_t key)
{
	if constexpr (page == 16384)
		return key & (::uml::memory::tlb_set_count - 1);
	else
		return (key ^ (key >> 13)) & (::uml::memory::tlb_set_count - 1);
}

[[nodiscard]] std::uint32_t colliding_page(std::uint32_t target,
	unsigned ordinal)
{
	const std::uint64_t target_key = target >> DARWIN_UML_GUEST_PAGE_SHIFT;
	unsigned seen = 0;
	for (std::uint64_t key = target_key + 1;; ++key) {
		if (tlb_set(key) != tlb_set(target_key))
			continue;
		if (++seen == ordinal)
			return static_cast<std::uint32_t>(
				key << DARWIN_UML_GUEST_PAGE_SHIFT);
	}
}

/* A program assembled instruction by instruction: an A32 word is four
 * bytes; a T32 value up to 0xffff is a halfword and anything above is a
 * 32-bit instruction, first halfword in the high half. */
class Code final {
public:
	explicit Code(bool thumb) : thumb_{thumb} {}

	void add(std::uint32_t instruction)
	{
		if (!thumb_) {
			bytes_ += 4;
			words_.push_back(instruction);
		} else if (instruction > 0xffffU) {
			bytes_ += 4;
			words_.push_back(instruction);
		} else {
			bytes_ += 2;
			words_.push_back(instruction);
		}
	}
	[[nodiscard]] std::uint32_t size() const noexcept { return bytes_; }

	/* A conditional branch back to the start, taken when NE. */
	void branch_back_ne()
	{
		if (thumb_) {
			const auto back = -static_cast<std::int32_t>(bytes_ + 4);
			add(0xd100U | (static_cast<std::uint32_t>(back / 2) & 0xffU));
		} else {
			const auto back = -static_cast<std::int32_t>(bytes_ + 8) / 4;
			add(0x1a00'0000U | (static_cast<std::uint32_t>(back) & 0xff'ffffU));
		}
	}

	void install(Guest &guest) const
	{
		std::uint32_t offset = 0;
		for (const auto word : words_) {
			if (!thumb_) {
				guest.a32(offset, {word});
				offset += 4;
			} else if (word > 0xffffU) {
				guest.t32(offset, {static_cast<std::uint16_t>(word >> 16),
						   static_cast<std::uint16_t>(word)});
				offset += 4;
			} else {
				guest.t32(offset, {static_cast<std::uint16_t>(word)});
				offset += 2;
			}
		}
	}

private:
	bool thumb_;
	std::uint32_t bytes_{};
	std::vector<std::uint32_t> words_;
};

void exclusive_loops_progress_with_colliding_pages()
{
	enum class Form { word, byte, half, pair };
	unsigned long generation = 1000;
	for (const bool thumb : {false, true})
	for (const bool inside : {false, true})
	for (const auto form : {Form::word, Form::byte, Form::half, Form::pair}) {
		/* The byte and halfword forms are exercised in A32; T32 has
		 * the same gadgets behind a different decoder. */
		if (thumb && (form == Form::byte || form == Form::half))
			continue;
		Guest guest{generation++};
		const std::uint32_t first = colliding_page(data_base, 1);
		const std::uint32_t second = colliding_page(data_base, 2);
		guest.map_data(first);
		guest.map_data(second);

		/* r2 is the target, r5 and r8 the pages that collide with it,
		 * r4 the value stored there. */
		Code code{thumb};
		const auto plain_stores = [&] {
			code.add(thumb ? 0x602cU : 0xe585'4000U);
			code.add(thumb ? 0xf8c8'4000U : 0xe588'4000U);
		};
		if (!inside)
			plain_stores();
		switch (form) {
		case Form::word:
			code.add(thumb ? 0xe852'1f00U : a::ldrex_r1_r2);
			break;
		case Form::byte:
			code.add(0xe1d2'1f9fU); /* ldrexb r1,[r2] */
			break;
		case Form::half:
			code.add(0xe1f2'1f9fU); /* ldrexh r1,[r2] */
			break;
		case Form::pair:
			code.add(thumb ? 0xe8d2'677fU : 0xe1b2'6f9fU);
			break; /* ldrexd r6,r7,[r2] */
		}
		if (inside)
			plain_stores();
		if (form == Form::pair) {
			code.add(thumb ? 0x1c76U : 0xe296'6001U); /* adds r6,r6,#1 */
			code.add(thumb ? 0xf147'0700U : 0xe2a7'7000U); /* adc r7,r7,#0 */
		} else {
			code.add(thumb ? 0x1c49U : 0xe281'1001U); /* add r1,r1,#1 */
		}
		switch (form) {
		case Form::word:
			code.add(thumb ? 0xe842'1300U : a::strex_r3_r1_r2);
			break;
		case Form::byte:
			code.add(0xe1c2'3f91U); /* strexb r3,r1,[r2] */
			break;
		case Form::half:
			code.add(0xe1e2'3f91U); /* strexh r3,r1,[r2] */
			break;
		case Form::pair:
			code.add(thumb ? 0xe8c2'6773U : 0xe1a2'3f96U);
			break; /* strexd r3,r6,r7,[r2] */
		}
		code.add(thumb ? t::cmp_r3_0 : a::cmp_r3_0);
		code.branch_back_ne();
		code.add(thumb ? t::svc : a::svc);
		code.install(guest);

		guest.data32(0x20, 41);
		guest.data32(0x24, 1000);
		auto cpu = thumb ? t32_state() : a32_state();
		cpu.registers[2] = data_base + 0x20;
		cpu.registers[4] = 0x5a;
		cpu.registers[5] = first + 0x10;
		cpu.registers[8] = second + 0x10;
		const auto result = guest.run(cpu, 20'000);
		require(result.reason == ExitReason::system_call);
		require(reg(cpu, 3) == 0);
		switch (form) {
		case Form::word:
			require(guest.data32(0x20) == 42);
			break;
		case Form::byte:
			require((guest.data32(0x20) & 0xff) == 42);
			break;
		case Form::half:
			require((guest.data32(0x20) & 0xffff) == 42);
			break;
		case Form::pair:
			/* The low word was incremented. */
			require(guest.data32(0x20) == 42 && guest.data32(0x24) == 1000);
			break;
		}
	}
}

void a_misaligned_exclusive_faults_as_an_alignment_fault()
{
	Guest guest{969};
	guest.a32(0, {a::mov_r0_1, a::ldrex_r1_r2, a::svc});
	auto cpu = a32_state();
	cpu.registers[2] = data_base + 0x22;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::memory_fault);
	require(result.fault.alignment);
	require(result.fault.address == data_base + 0x22);
	require(result.retired_instructions == 1);
	require(cpu.pc == aarch32::a32_key(code_base + 4));
}

void coprocessor_and_counter_reads()
{
	Guest guest{970};
	guest.a32(0, {a::mrc_tpidruro, 0xe1a0'4000U /* mov r4,r0 */,
		      a::mrc_tpidrurw, 0xe1a0'5000U /* mov r5,r0 */,
		      0xe3a0'0033U /* mov r0,#0x33 */, a::mcr_tpidrurw,
		      a::mrc_tpidrurw, 0xe1a0'6000U /* mov r6,r0 */,
		      a::mrc_cntfrq, 0xe1a0'7000U /* mov r7,r0 */,
		      a::mrrc_cntvct, 0xe1a0'8000U /* mov r8,r0 */, a::svc});
	auto cpu = a32_state();
	cpu.a32_tpidruro = 0xb6f0'0010U;
	cpu.tls = 0x7000'1000U;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::system_call);
	require(reg(cpu, 4) == 0xb6f0'0010U);
	require(reg(cpu, 5) == 0x7000'1000U);
	require(reg(cpu, 6) == 0x33);
	require(static_cast<std::uint32_t>(cpu.tls) == 0x33);
	require(reg(cpu, 7) != 0);
	/* CNTVCT is a 64-bit count that has started. */
	require((std::uint64_t{reg(cpu, 1)} << 32 | reg(cpu, 8)) != 0);

	/* TPIDRURO is not writable from EL0. */
	Guest read_only{971};
	read_only.a32(0, {a::mov_r0_1, a::mcr_tpidruro, a::svc});
	auto write = a32_state();
	const auto denied = read_only.run(write);
	require(denied.reason == ExitReason::illegal_instruction);
	require(denied.retired_instructions == 1);
	require(write.pc == aarch32::a32_key(code_base + 4));
}

void the_counter_does_not_run_backwards()
{
	Guest guest{972};
	guest.a32(0, {a::mrrc_cntvct, 0xe1a0'4000U, 0xe1a0'5001U,
		      a::mrrc_cntvct, a::svc});
	auto cpu = a32_state();
	require(guest.run(cpu).reason == ExitReason::system_call);
	const auto first = std::uint64_t{reg(cpu, 5)} << 32 | reg(cpu, 4);
	const auto second = std::uint64_t{reg(cpu, 1)} << 32 | reg(cpu, 0);
	require(second >= first);
}

void cp15_barriers_follow_the_enable()
{
	for (const bool enabled : {false, true}) {
		Guest guest{enabled ? 973UL : 974UL};
		guest.a32(0, {a::mov_r0_1, a::mcr_dmb, a::svc});
		auto cpu = a32_state();
		cpu.a32_cp15ben = enabled ? 1 : 0;
		const auto result = guest.run(cpu);
		if (enabled) {
			require(result.reason == ExitReason::system_call);
			require(result.retired_instructions == 3);
		} else {
			require(result.reason == ExitReason::illegal_instruction);
			require(result.retired_instructions == 1);
			require(cpu.pc == aarch32::a32_key(code_base + 4));
		}
	}
}

void reading_the_status_register_gives_flags_q_ge_and_user_mode()
{
	Guest guest{975};
	guest.a32(0, {a::mrs_apsr, a::svc});
	auto cpu = a32_state();
	cpu.nzcv = 0x6000'0000U;
	cpu.a32_q = 1;
	cpu.a32_ge_mask = 0x00ff'00ffU; /* GE[2] and GE[0] */
	require(guest.run(cpu).reason == ExitReason::system_call);
	require(reg(cpu, 0) == (0x6000'0000U | 1U << 27 | 0b0101U << 16 | 0x10U));
}

/* ------------------------------------------------------------ pages */

void block_transfers_cross_a_page_boundary()
{
	Guest guest{976};
	/* ldm r0,{r1-r4} ; add r1,r1,#256 ; stm r0!,{r1-r4} : the load and
	 * the store both straddle the boundary between the two data pages. */
	guest.a32(0, {a::ldm_r0_r1_r4, 0xe281'1c01U, a::stm_r0_wb_r1_r4, a::svc});
	const std::uint32_t boundary = page - 8;
	for (std::uint32_t index = 0; index < 4; ++index)
		guest.data32(boundary + 4 * index, 0x1000'0000U + index);
	auto cpu = a32_state();
	cpu.registers[0] = data_base + boundary;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::system_call);
	require(result.retired_instructions == 4);
	require(reg(cpu, 1) == 0x1000'0100U && reg(cpu, 4) == 0x1000'0003U);
	require(reg(cpu, 0) == data_base + boundary + 16);
	require(guest.data32(boundary) == 0x1000'0100U);
	for (std::uint32_t index = 1; index < 4; ++index)
		require(guest.data32(boundary + 4 * index) ==
			0x1000'0000U + index);
}

void a_double_word_load_crosses_a_page_boundary()
{
	Guest guest{977};
	guest.a32(0, {a::ldrd_r2_r0, a::svc});
	guest.data32(page - 4, 0xaaaa'0001U);
	guest.data32(page, 0xbbbb'0002U);
	auto cpu = a32_state();
	cpu.registers[0] = data_base + page - 4;
	require(guest.run(cpu).reason == ExitReason::system_call);
	require(reg(cpu, 2) == 0xaaaa'0001U && reg(cpu, 3) == 0xbbbb'0002U);
}

void a_block_transfer_into_an_unmapped_page_faults_at_that_page()
{
	/* The data ends where the second data page ends; the next page is
	 * not mapped. */
	const std::uint32_t last = data_pages * page - 8;
	{
		Guest guest{978};
		guest.a32(0, {a::stm_r0_wb_r1_r4, a::svc});
		auto cpu = a32_state();
		cpu.registers[0] = data_base + last;
		cpu.registers[1] = 0x11;
		cpu.registers[2] = 0x22;
		cpu.registers[3] = 0x33;
		cpu.registers[4] = 0x44;
		guest.data32(last, 0xdead'0000U);
		guest.data32(last + 4, 0xdead'0004U);
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::memory_fault);
		require(result.retired_instructions == 0);
		require(cpu.pc == aarch32::a32_key(code_base));
		require(result.fault.access == ::uml::memory::MemoryAccess::write);
		/* The fault is at the first word in the page that is not
		 * mapped. */
		require(result.fault.address == data_base + data_pages * page);
		/* The base is not written back, so the instruction can be
		 * restarted; the words ahead of the fault are in memory, as
		 * a sequence of word stores leaves them. */
		require(reg(cpu, 0) == data_base + last);
		require(guest.data32(last) == 0x11);
		require(guest.data32(last + 4) == 0x22);
	}
	{
		Guest guest{979};
		guest.a32(0, {a::ldm_r0_r1_r4, a::svc});
		auto cpu = a32_state();
		cpu.registers[0] = data_base + last;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::memory_fault);
		require(result.retired_instructions == 0);
		require(result.fault.access == ::uml::memory::MemoryAccess::read);
		require(result.fault.address >= data_base + data_pages * page);
		require(reg(cpu, 0) == data_base + last);
	}
}

} // namespace

int main()
{
	const ::uml::testing::RuntimeTestRoute runtime_route;
	straight_line_retires_every_instruction();
	skipped_conditional_instructions_retire();
	conditional_branch_retires_taken_or_not();
	conditional_supervisor_call_is_skipped_or_taken();
	a_data_fault_stops_at_the_instruction();
	a_fault_inside_an_it_block_keeps_its_itstate();
	a_supervisor_call_inside_an_it_block_resumes_mid_block();
	an_undefined_instruction_does_not_retire();
	a_conditional_undefined_word_is_skipped_when_its_condition_fails();
	a_breakpoint_reports_its_immediate();
	interworking_branches_switch_state_both_ways();
	an_a32_target_with_bit_one_set_is_a_pc_alignment_exit();
	a_fetch_fault_reports_the_unfetchable_address();
	the_pc_reads_as_the_architectural_value();
	the_instruction_budget_ends_a_loop();
	single_step_retires_one_instruction_at_a_time();
	single_step_walks_an_it_block();
	changed_code_is_translated_again();
	a_a32_block_and_a_t32_block_at_one_address_are_distinct();
	exclusive_loops_complete_and_store_once();
	a_store_without_a_reservation_fails();
	exclusive_loops_progress_with_colliding_pages();
	a_misaligned_exclusive_faults_as_an_alignment_fault();
	coprocessor_and_counter_reads();
	the_counter_does_not_run_backwards();
	cp15_barriers_follow_the_enable();
	reading_the_status_register_gives_flags_q_ge_and_user_mode();
	block_transfers_cross_a_page_boundary();
	a_double_word_load_crosses_a_page_boundary();
	a_block_transfer_into_an_unmapped_page_faults_at_that_page();
	std::puts("arm64-gadget-a32-control: ok");
	return 0;
}
