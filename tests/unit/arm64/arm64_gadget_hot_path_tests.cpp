// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <utility>

namespace gadgets = uml::cpu::arm64::gadgets;

extern "C" {
void uml_arm64_gadget_nop();
void uml_arm64_gadget_svc();
void uml_arm64_gadget_branch_register();
void uml_arm64_gadget_branch_link_register();
void uml_arm64_gadget_return();
void uml_arm64_gadget_pc_relative();
void uml_arm64_gadget_move_wide_constant();
void uml_arm64_gadget_move_wide_keep_x1();
void uml_arm64_gadget_add_sub_immediate_x_add_n_rr();
void uml_arm64_gadget_add_sub_immediate_x_add_s_ss();
void uml_arm64_gadget_add_sub_carry_x_adc_n_rr_r();
void uml_arm64_gadget_variable_shift_x_lsl_rr_r();
void uml_arm64_gadget_divide_w_s_rr_r();
void uml_arm64_gadget_load8_unsigned_fast_r();
void uml_arm64_gadget_load64_unsigned_fast_r();
void uml_arm64_gadget_vector_load128_unsigned_fast();
void uml_arm64_gadget_literal_load64_fast_r();
void uml_arm64_gadget_scalar_other_load32u_pre_r();
void uml_arm64_gadget_vector_other_load128_pre();
void uml_arm64_gadget_integer_pair_load64_1_rr();
void uml_arm64_gadget_vector_pair_load128_2();
void uml_arm64_gadget_simd_structure_contig_q_2_load_1();
void uml_arm64_gadget_simd_replicate_s_q_3_0();
void uml_arm64_gadget_simd_lane_h_4_load_2();
void uml_arm64_gadget_ordered_writeback_load_x_r();
void uml_arm64_gadget_atomic_ldadd_relaxed_x_rr();
void uml_arm64_gadget_exclusive_load_relaxed_x_r();
void uml_arm64_gadget_simd_three_same_sshl_8b();
void uml_arm64_gadget_simd_three_same_urshl_2d();
void uml_arm64_gadget_simd_three_same_smaxp_16b();
void uml_arm64_gadget_simd_three_same_add_d();
void uml_arm64_gadget_simd_three_same_mul_4s();
void uml_arm64_gadget_simd_three_same_cmeq_8h();
void uml_arm64_gadget_simd_unary_xtn_8h2();
}

static void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

template<typename Function>
static std::uintptr_t address_of(Function function)
{
	return reinterpret_cast<std::uintptr_t>(function);
}

template<typename Function>
static void require_gadget(const gadgets::Program &program, Function expected)
{
	const auto address = address_of(expected);
	if (program.words()[0] != address)
		std::fprintf(stderr, "unexpected gadget: actual=%#lx expected=%#lx\n",
			static_cast<unsigned long>(program.words()[0]),
			static_cast<unsigned long>(address));
	require(program.words()[0] == address);
}
static gadgets::Program build_one(std::uint32_t instruction)
{
	constexpr std::uint64_t base = 0x4000;
	const std::array instructions{instruction, 0xd400'0001U};
	auto built = gadgets::Builder{}.build(base, instructions);
	require(built.complete);
	return std::move(built.program);
}

static void require_predecoded(const gadgets::Program &program,
	std::uint32_t instruction)
{
	require(program.words()[1] != instruction);
	require(program.words()[2] == 0x4000U);
}

static constexpr std::uint32_t unsigned_immediate(unsigned size,
	unsigned operation, unsigned immediate, unsigned base,
	unsigned transfer, bool vector = false)
{
	return 0x3900'0000U | ((size & 3U) << 30) |
		((operation & 3U) << 22) | (static_cast<unsigned>(vector) << 26) |
		((immediate & 0xfffU) << 10) | ((base & 31U) << 5) |
		(transfer & 31U);
}

static constexpr std::uint32_t immediate_other(unsigned size,
	unsigned operation, unsigned immediate, unsigned mode, unsigned base,
	unsigned transfer, bool vector = false)
{
	return 0x3800'0000U | ((size & 3U) << 30) |
		((operation & 3U) << 22) | (static_cast<unsigned>(vector) << 26) |
		((immediate & 0x1ffU) << 12) | ((mode & 3U) << 10) |
		((base & 31U) << 5) | (transfer & 31U);
}

static constexpr std::uint32_t pair(unsigned operation, bool vector,
	bool load, unsigned mode, unsigned immediate, unsigned base,
	unsigned first, unsigned second)
{
	return 0x2800'0000U | ((operation & 3U) << 30) |
		(static_cast<unsigned>(vector) << 26) |
		(static_cast<unsigned>(load) << 22) | ((mode & 3U) << 23) |
		((immediate & 0x7fU) << 15) | ((second & 31U) << 10) |
		((base & 31U) << 5) | (first & 31U);
}

static constexpr std::uint32_t simd_multiple(bool q, bool load,
	bool post_index, unsigned offset, unsigned opcode, unsigned size,
	unsigned base, unsigned first)
{
	return 0x0c00'0000U | (static_cast<unsigned>(q) << 30) |
		(static_cast<unsigned>(post_index) << 23) |
		(static_cast<unsigned>(load) << 22) | ((offset & 31U) << 16) |
		((opcode & 15U) << 12) | ((size & 3U) << 10) |
		((base & 31U) << 5) | (first & 31U);
}

static constexpr std::uint32_t simd_single(bool q, bool load,
	bool post_index, unsigned extra, unsigned offset, unsigned opcode,
	unsigned scale_bit, unsigned size, unsigned base, unsigned first)
{
	return 0x0d00'0000U | (static_cast<unsigned>(q) << 30) |
		(static_cast<unsigned>(post_index) << 23) |
		(static_cast<unsigned>(load) << 22) | ((extra & 1U) << 21) |
		((offset & 31U) << 16) | ((opcode & 7U) << 13) |
		((scale_bit & 1U) << 12) | ((size & 3U) << 10) |
		((base & 31U) << 5) | (first & 31U);
}

static constexpr std::uint32_t atomic(unsigned size, unsigned operation,
	unsigned base, unsigned source, unsigned destination)
{
	return 0x3820'0000U | ((size & 3U) << 30) |
		((source & 31U) << 16) | ((operation & 7U) << 12) |
		((base & 31U) << 5) | (destination & 31U);
}

static constexpr std::uint32_t exclusive_load(unsigned size,
	unsigned base, unsigned transfer)
{
	return 0x0800'0000U | ((size & 3U) << 30) | (1U << 22) |
		(31U << 16) | (31U << 10) | ((base & 31U) << 5) |
		(transfer & 31U);
}

static constexpr std::uint32_t three_same(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode,
	unsigned destination = 0, unsigned left = 1, unsigned right = 2)
{
	return (scalar ? 0x5e20'0400U : 0x0e20'0400U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((opcode & 31U) << 11) |
		((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

static constexpr std::uint32_t two_register(bool q, unsigned size,
	unsigned opcode, unsigned destination = 0, unsigned source = 1)
{
	return 0x0e20'0800U | (q ? 0x4000'0000U : 0U) |
		((size & 3U) << 22) | ((opcode & 31U) << 12) |
		((source & 31U) << 5) | (destination & 31U);
}

int main()
{
	{
		const auto program = build_one(0xd61f'0060U); // br x3
		require_gadget(program, uml_arm64_gadget_branch_register);
		require(program.words()[1] == 3U * 8U);
	}
	{
		const auto program = build_one(0xd63f'0080U); // blr x4
		require_gadget(program, uml_arm64_gadget_branch_link_register);
		require(program.words()[1] == 4U * 8U);
		require(program.words()[2] == 0x4004U);
	}
	{
		const auto program = build_one(0xd65f'03c0U); // ret x30
		require_gadget(program, uml_arm64_gadget_return);
		require(program.words()[1] == 30U * 8U);
	}
	{
		const auto program = build_one(0x1000'0002U); // adr x2, pc
		require_gadget(program, uml_arm64_gadget_pc_relative);
		require(program.words()[1] == 2U * 8U);
		require(program.words()[2] == 0x4000U);
	}
	{
		const auto program = build_one(0x1000'001fU); // adr xzr, pc
		require_gadget(program, uml_arm64_gadget_nop);
		/* No discarded destination operand/value is left in the stream. */
		require(program.words()[1] == address_of(uml_arm64_gadget_svc));
	}
	{
		const auto program = build_one(0xd280'00a0U); // movz x0, #5
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_move_wide_constant));
		require(program.words()[1] == 0U);
		require(program.words()[2] == 5U);
	}
	{
		const auto program = build_one(0xf2a2'4680U); // movk x0,#0x1234,lsl#16
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_move_wide_keep_x1));
	}
	{
		const auto program = build_one(0x9100'1c01U); // add x1,x0,#7
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_add_sub_immediate_x_add_n_rr));
	}
	{
		const auto program = build_one(0xb100'07ffU); // adds xzr,sp,#1
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_add_sub_immediate_x_add_s_ss));
	}
	{
		const auto program = build_one(0x9a04'0062U); // adc x2,x3,x4
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_add_sub_carry_x_adc_n_rr_r));
	}
	{
		const auto program = build_one(0x9ac4'2062U); // lslv x2,x3,x4
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_variable_shift_x_lsl_rr_r));
	}
	{
		const auto program = build_one(0x1ac4'0c62U); // sdiv w2,w3,w4
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_divide_w_s_rr_r));
	}
	{
		const auto program = build_one(0xf940'0462U); // ldr x2,[x3,#8]
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_load64_unsigned_fast_r));
		/* The opcode is not carried into the runtime stream: operand then PC. */
		require(program.words()[2] == 0x4000U);
	}
	{
		const auto instruction = unsigned_immediate(0, 1, 1, 3, 2);
		const auto program = build_one(instruction); // ldrb w2,[x3,#1]
		require_gadget(program, uml_arm64_gadget_load8_unsigned_fast_r);
		require_predecoded(program, instruction);
	}
	{
		const auto instruction = unsigned_immediate(0, 3, 1, 3, 2, true);
		const auto program = build_one(instruction); // ldr q2,[x3,#16]
		require_gadget(program, uml_arm64_gadget_vector_load128_unsigned_fast);
		require_predecoded(program, instruction);
	}
	{
		constexpr std::uint32_t instruction = 0x5800'0042U; // ldr x2,pc+8
		const auto program = build_one(instruction);
		require_gadget(program, uml_arm64_gadget_literal_load64_fast_r);
		require_predecoded(program, instruction);
	}
	{
		const auto instruction = immediate_other(2, 1, 1, 3, 3, 2);
		const auto program = build_one(instruction); // ldr w2,[x3,#1]!
		require_gadget(program, uml_arm64_gadget_scalar_other_load32u_pre_r);
		require_predecoded(program, instruction);
	}
	{
		const auto instruction = immediate_other(0, 3, 1, 3, 3, 2, true);
		const auto program = build_one(instruction); // ldr q2,[x3,#1]!
		require_gadget(program, uml_arm64_gadget_vector_other_load128_pre);
		require_predecoded(program, instruction);
	}
	{
		const auto instruction = pair(2, false, true, 3, 1, 3, 2, 4);
		const auto program = build_one(instruction); // ldp x2,x4,[x3,#8]!
		require_gadget(program, uml_arm64_gadget_integer_pair_load64_1_rr);
		require_predecoded(program, instruction);
	}
	{
		const auto instruction = pair(2, true, true, 1, 1, 3, 2, 4);
		const auto program = build_one(instruction); // ldp q2,q4,[x3],#16
		require_gadget(program, uml_arm64_gadget_vector_pair_load128_2);
		require_predecoded(program, instruction);
	}
	{
		const auto instruction = simd_multiple(true, true, true,
			31, 10, 0, 3, 2); // ld1 {v2.16b,v3.16b},[x3],#32
		const auto program = build_one(instruction);
		require_gadget(program,
			uml_arm64_gadget_simd_structure_contig_q_2_load_1);
		require_predecoded(program, instruction);
	}
	{
		const auto instruction = simd_single(true, true, false,
			0, 0, 7, 0, 2, 3, 2); // ld3r {v2.4s-v4.4s},[x3]
		const auto program = build_one(instruction);
		require_gadget(program, uml_arm64_gadget_simd_replicate_s_q_3_0);
		require_predecoded(program, instruction);
	}
	{
		const auto instruction = simd_single(true, true, true,
			1, 4, 3, 0, 0, 3, 2); // ld4 lanes, post-index x4
		const auto program = build_one(instruction);
		require_gadget(program, uml_arm64_gadget_simd_lane_h_4_load_2);
		require_predecoded(program, instruction);
	}
	{
		constexpr std::uint32_t instruction =
			0x1980'0800U | (3U << 30) | (1U << 22) | (3U << 5) | 2U;
		const auto program = build_one(instruction); // ldapr x2,[x3],#8
		require_gadget(program, uml_arm64_gadget_ordered_writeback_load_x_r);
		require_predecoded(program, instruction);
	}
	{
		const auto instruction = atomic(3, 0, 3, 4, 2);
		const auto program = build_one(instruction); // ldadd x4,x2,[x3]
		require_gadget(program, uml_arm64_gadget_atomic_ldadd_relaxed_x_rr);
		require_predecoded(program, instruction);
	}
	{
		const auto instruction = exclusive_load(3, 3, 2);
		const auto program = build_one(instruction); // ldxr x2,[x3]
		require_gadget(program, uml_arm64_gadget_exclusive_load_relaxed_x_r);
		require_predecoded(program, instruction);
	}
	{
		const auto program = build_one(three_same(false, false, false, 0, 8));
		require_gadget(program, uml_arm64_gadget_simd_three_same_sshl_8b);
	}
	{
		const auto program = build_one(three_same(false, true, true, 3, 10));
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_simd_three_same_urshl_2d));
	}
	{
		const auto program = build_one(three_same(false, true, false, 0, 20));
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_simd_three_same_smaxp_16b));
	}
	{
		const auto program = build_one(three_same(true, false, false, 3, 16));
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_simd_three_same_add_d));
	}
	{
		const auto program = build_one(three_same(false, true, false, 2, 19));
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_simd_three_same_mul_4s));
	}
	{
		const auto program = build_one(three_same(false, true, true, 1, 17));
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_simd_three_same_cmeq_8h));
	}
	{
		const auto program = build_one(two_register(true, 0, 18)); // XTN2
		require(program.words()[0] ==
			address_of(uml_arm64_gadget_simd_unary_xtn_8h2));
	}
	return 0;
}
