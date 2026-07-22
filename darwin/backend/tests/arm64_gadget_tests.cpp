// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/detail/instruction_helpers.hpp"

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <mach/mach.h>
#include <memory>
#include <source_location>
#include <sys/mman.h>
#include <unistd.h>

#include "uml_darwin_static_mmu.h"

namespace gadgets = uml::backend::arm64::gadgets;

class NullMemory final : public uml::backend::arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t, std::span<std::byte>) override
	{
		return false;
	}
	[[nodiscard]] bool write(
		std::uint64_t, std::span<const std::byte>) override
	{
		return false;
	}
};

static void require(bool condition,
		    const std::source_location location =
			    std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

static void require_data_only(const gadgets::Program &program)
{
	vm_address_t address = reinterpret_cast<vm_address_t>(
		program.words().data());
	vm_size_t size = 0;
	vm_region_basic_info_data_64_t info{};
	mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object = MACH_PORT_NULL;
	const auto result = vm_region_64(mach_task_self(), &address, &size,
		VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info),
		&count, &object);
	require(result == KERN_SUCCESS);
	require((info.protection & VM_PROT_EXECUTE) == 0);
	if (object != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), object);
}

static std::uint64_t read_fpcr()
{
	std::uint64_t value;
	asm volatile("mrs %0, fpcr" : "=r"(value));
	return value;
}

static std::uint64_t read_fpsr()
{
	std::uint64_t value;
	asm volatile("mrs %0, fpsr" : "=r"(value));
	return value;
}

static void write_fpcr(std::uint64_t value)
{
	asm volatile("msr fpcr, %0" : : "r"(value));
}

static void write_fpsr(std::uint64_t value)
{
	asm volatile("msr fpsr, %0" : : "r"(value));
}

int main()
{
	constexpr std::uint64_t base = 0x4000;
	/* Pure SIMD gadgets are differential-tested against the architectural
	 * dispatcher. These are especially prone to aliasing and upper-half bugs. */
	const std::array simd_differential_instructions{
		0x6f00'0400U, // mvni v0.4s, #0 (must not alias USHR decode)
		0x4e20'9801U, // cmeq v1.16b, v0.16b, #0
		0x0f0c'8422U, // shrn v2.8b, v1.8h, #4
		0x0e21'4022U, // addhn v2.8b, v1.8h, v1.8h
		0x6e20'8c23U, // cmeq v3.16b, v1.16b, v0.16b
		0x6ea4'1c62U, // bit v2.16b, v3.16b, v4.16b
		0x6e22'a445U, // umaxp v5.16b, v2.16b, v2.16b
		0x4e22'bc45U, // addp v5.16b, v2.16b, v2.16b
		0x0e20'5bffU, // cnt v31.8b, v31.8b
		0x6e20'5bffU, // mvn v31.16b, v31.16b (must not alias CNT)
		0x0ea0'0bdeU, // rev64 v30.2s, v30.2s
		0x6e20'0bffU, // rev32 v31.16b, v31.16b
		0x4e20'1bffU, // rev16 v31.16b, v31.16b
		0x0e31'bbffU, // addv b31, v31.8b
		0x6e1f'43ffU, // ext v31.16b, v31.16b, v31.16b, #8
		0x6f0d'0401U, // ushr v1.16b, v0.16b, #3
		0x4e01'2080U, // tbl v0.16b, {v4.16b, v5.16b}, v1.16b
		0x0e02'07ffU, // dup v31.4h, v31.h[0]
		0x0e14'3fe1U, // mov w1, v31.s[2]
		0x4e18'1e9fU, // mov v31.d[1], x20
		0x4e22'4400U, // sshl v0.16b, v0.16b, v2.16b
		0x4e20'a800U, // cmlt v0.16b, v0.16b, #0
		0x1e20'23f8U, // fcmpe s31, #0.0
		0x1e22'c1e1U, // fcvt d1, s15
		0x1e23'027eU, // ucvtf s30, w19
		0x1e39'03a0U, // fcvtzu w0, s29
		0x5ee0'9bffU, // cmeq d31, d31, #0
		0x5e61'dad6U, // scvtf d22, d22
		0x0ebe'13ffU, // saddw v31.2d, v31.2d, v30.2s
		0x1e6f'0400U, // fccmp d0, d15, #0, eq
		0x1e7f'1404U, // fccmp d0, d31, #4, ne
		0x1e66'43fdU, // frinta d29, d31
		0x6ee0'fad6U, // fneg v22.2d, v22.2d
	};
	for (const auto instruction : simd_differential_instructions) {
		uml::backend::arm64::CpuState initial{};
		initial.pc = base;
		for (std::size_t reg = 0; reg < initial.vectors.size(); ++reg)
			for (std::size_t byte = 0;
			     byte < initial.vectors[reg].bytes.size(); ++byte)
				initial.vectors[reg].bytes[byte] = static_cast<std::byte>(
					reg * 17 + byte * 13 + 5);
		auto expected = initial;
		NullMemory memory;
		const auto dispatched = uml::backend::arm64::Dispatcher{}.execute(
			expected, memory, instruction);
		require(dispatched.reason ==
			uml::backend::arm64::StopReason::continued);
		const std::array stream{instruction, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(base, stream);
		require(built.complete);
		auto actual = initial;
		auto differential_tlb = std::make_unique<gadgets::Tlb>();
		differential_tlb->prepare(0, darwin_uml_static_vm_changes(0));
		require(gadgets::execute(built.program, actual, *differential_tlb) ==
			gadgets::Event::system_call);
		if (std::memcmp(actual.vectors.data(), expected.vectors.data(),
		    sizeof(actual.vectors)) != 0 || actual.fpsr != expected.fpsr ||
		    actual.nzcv != expected.nzcv ||
		    actual.registers != expected.registers) {
			std::fprintf(stderr, "SIMD differential mismatch: 0x%08x\n",
				instruction);
			std::abort();
		}
	}
	for (const unsigned lane_bits : {8U, 16U, 32U, 64U}) {
		for (const bool q : {false, true}) {
			if (lane_bits == 64 && !q)
				continue;
			for (const bool is_unsigned : {false, true}) {
				for (unsigned shift = 1; shift <= lane_bits; ++shift) {
					const auto encoded = 2 * lane_bits - shift;
					const std::uint32_t instruction = 0x0f00'0400U |
						(q ? 0x4000'0000U : 0) |
						(is_unsigned ? 0x2000'0000U : 0) |
						(encoded << 16) | (3U << 5) | 5U;
					uml::backend::arm64::CpuState initial{};
					initial.pc = base;
					for (std::size_t byte = 0; byte < 16; ++byte)
						initial.vectors[3].bytes[byte] =
							static_cast<std::byte>(byte * 29 + 0x81);
					auto expected = initial;
					NullMemory memory;
					const auto dispatched =
						uml::backend::arm64::Dispatcher{}.execute(
							expected, memory, instruction);
					require(dispatched.reason ==
						uml::backend::arm64::StopReason::continued);
					const std::array stream{instruction, 0xd400'0001U};
					const auto built = gadgets::Builder{}.build(base, stream);
					require(built.complete);
					auto actual = initial;
					auto test_tlb = std::make_unique<gadgets::Tlb>();
					test_tlb->prepare(0, darwin_uml_static_vm_changes(0));
					require(gadgets::execute(
						built.program, actual, *test_tlb) ==
						gadgets::Event::system_call);
					if (std::memcmp(actual.vectors.data(),
					    expected.vectors.data(), sizeof(actual.vectors)) != 0) {
						std::fprintf(stderr,
							"SIMD shift mismatch: 0x%08x\n", instruction);
						std::abort();
					}
				}
			}
		}
	}
	for (unsigned operation = 0; operation < 8; ++operation) {
		const std::uint32_t instruction = 0x1ac0'4000U |
			(operation << 10) | (4U << 16) | (3U << 5) | 5U |
			((operation & 3U) == 3U ? 0x8000'0000U : 0U);
		uml::backend::arm64::CpuState expected{};
		expected.pc = base;
		expected.registers[3] = 0x89ab'cdefU;
		expected.registers[4] = 0x0123'4567'89ab'cdefULL;
		auto actual = expected;
		NullMemory memory;
		require(uml::backend::arm64::Dispatcher{}.execute(
			expected, memory, instruction).reason ==
			uml::backend::arm64::StopReason::continued);
		const std::array stream{instruction, 0xd400'0001U};
		const auto program = gadgets::Builder{}.build(base, stream);
		require(program.complete);
		auto crc_tlb = std::make_unique<gadgets::Tlb>();
		crc_tlb->prepare(0, darwin_uml_static_vm_changes(0));
		require(gadgets::execute(program.program, actual, *crc_tlb) ==
			gadgets::Event::system_call);
		require(actual.registers == expected.registers);
	}
	{
		constexpr std::uint32_t instruction = 0x93c1'8021U;
		uml::backend::arm64::CpuState expected{};
		expected.pc = base;
		expected.registers[1] = 0x0123'4567'89ab'cdefULL;
		auto actual = expected;
		NullMemory memory;
		require(uml::backend::arm64::Dispatcher{}.execute(
			expected, memory, instruction).reason ==
			uml::backend::arm64::StopReason::continued);
		const std::array stream{instruction, 0xd400'0001U};
		const auto program = gadgets::Builder{}.build(base, stream);
		require(program.complete);
		auto extract_tlb = std::make_unique<gadgets::Tlb>();
		extract_tlb->prepare(0, darwin_uml_static_vm_changes(0));
		require(gadgets::execute(program.program, actual, *extract_tlb) ==
			gadgets::Event::system_call);
		require(actual.registers == expected.registers);
	}
	const std::array instructions{
		0xd280'00a0U, // movz x0, #5
		0xf2a2'4680U, // movk x0, #0x1234, lsl #16
		0x9100'1c01U, // add x1, x0, #7
		0xd503'201fU, // nop
		0xd400'0001U, // svc #0
	};
	const auto built = gadgets::Builder{}.build(base, instructions);
	require(built.complete);
	require(built.program.instruction_count() == instructions.size());
	require_data_only(built.program);

	uml::backend::arm64::CpuState cpu{};
	cpu.pc = base;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	const auto event = gadgets::execute(built.program, cpu, *tlb);
	require(event == gadgets::Event::system_call);
	require(cpu.registers[0] == 0x1234'0005ULL);
	require(cpu.registers[1] == 0x1234'000cULL);
	require(cpu.pc == base + instructions.size() * sizeof(std::uint32_t));

	const std::array core_integer_instructions{
		0x9282'4685U, // movn x5, #0x1234
		0xd100'83ffU, // sub sp, sp, #32
		0xb108'd4a6U, // adds x6, x5, #0x235
		0x9240'1cc7U, // and x7, x6, #0xff
		0xb278'00e8U, // orr x8, x7, #0x100
		0x1000'0009U, // adr x9, .
		0x9000'000aU, // adrp x10, current page
		0xd51b'd048U, // msr tpidr_el0, x8
		0xd53b'd04bU, // mrs x11, tpidr_el0
		0xd400'0001U, // svc #0
	};
	const auto core_integer = gadgets::Builder{}.build(
		base, core_integer_instructions);
	require(core_integer.complete);
	cpu = {};
	cpu.sp = 0x1000;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(core_integer.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[5] == ~std::uint64_t{0x1234});
	require(cpu.registers[6] == 0xffff'ffff'ffff'f000ULL);
	require(cpu.registers[7] == 0);
	require(cpu.registers[8] == 0x100);
	require(cpu.registers[9] == base + 5 * sizeof(std::uint32_t));
	require(cpu.registers[10] == (base & ~std::uint64_t{0xfff}));
	require(cpu.registers[11] == 0x100);
	require(cpu.sp == 0xfe0);
	require(cpu.nzcv == 0x8000'0000U);
	require(cpu.tls == 0x100);

	const std::array logical_immediate_flags_instructions{
		0x7201'041fU, // tst w0, #0x80000000
		0x927a'e53fU, // and sp, x9, #0xffffffffffffffc0
		0xd400'0001U, // svc #0
	};
	const auto logical_immediate_flags = gadgets::Builder{}.build(
		base, logical_immediate_flags_instructions);
	require(logical_immediate_flags.complete);
	cpu = {};
	cpu.registers[0] = 0x8000'0000U;
	cpu.registers[9] = 0x12'347U;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(logical_immediate_flags.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.nzcv == 0x8000'0000U);
	require(cpu.sp == 0x12'340U);

	const std::array register_arithmetic_instructions{
		0x8b07'0000U, // add x0, x0, x7
		0x8b20'4c60U, // add x0, x3, w0, uxtw #3
		0xeb07'0004U, // subs x4, x0, x7
		0xd400'0001U, // svc #0
	};
	const auto register_arithmetic = gadgets::Builder{}.build(
		base, register_arithmetic_instructions);
	require(register_arithmetic.complete);
	cpu = {};
	cpu.registers[0] = 5;
	cpu.registers[3] = 1;
	cpu.registers[7] = 7;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(register_arithmetic.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[0] == 97);
	require(cpu.registers[4] == 90);
	require(cpu.nzcv == 0x2000'0000U);

	const std::array carry_instructions{
		0x9a1f'01adU, // adc x13, x13, xzr
		0x3a02'0020U, // adcs w0, w1, w2
		0xda05'0083U, // sbc x3, x4, x5
		0xd400'0001U, // svc #0
	};
	const auto carry = gadgets::Builder{}.build(base, carry_instructions);
	require(carry.complete);
	cpu = {};
	cpu.nzcv = 0x2000'0000U;
	cpu.registers[13] = ~std::uint64_t{0};
	cpu.registers[1] = 0xffff'ffffU;
	cpu.registers[4] = 10;
	cpu.registers[5] = 3;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(carry.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[13] == 0);
	require(cpu.registers[0] == 0);
	require(cpu.registers[3] == 7);
	require(cpu.nzcv == 0x6000'0000U);

	const std::array multiply_shift_divide_instructions{
		0x9b01'0273U, // madd x19, x19, x1, x0
		0x9ba7'7c21U, // umull x1, w1, w7
		0x9ac4'2442U, // lsr x2, x2, x4
		0x1ac4'2803U, // asr w3, w0, w4
		0x9ac4'2c05U, // ror x5, x0, x4
		0x9ac6'08c8U, // udiv x8, x6, x6
		0x1ac6'0cc9U, // sdiv w9, w6, w6
		0xd400'0001U, // svc #0
	};
	const auto multiply_shift_divide = gadgets::Builder{}.build(
		base, multiply_shift_divide_instructions);
	require(multiply_shift_divide.complete);
	cpu = {};
	cpu.registers[0] = 7;
	cpu.registers[1] = 3;
	cpu.registers[2] = 0x100;
	cpu.registers[4] = 4;
	cpu.registers[6] = 21;
	cpu.registers[7] = 2;
	cpu.registers[19] = 5;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(multiply_shift_divide.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[19] == 22);
	require(cpu.registers[1] == 6);
	require(cpu.registers[2] == 0x10);
	require(cpu.registers[3] == 0);
	require(cpu.registers[5] == 0x7000'0000'0000'0000ULL);
	require(cpu.registers[8] == 1);
	require(cpu.registers[9] == 1);

	const std::array register_logical_instructions{
		0xaa00'03e5U, // mov x5, x0 (orr x5, xzr, x0)
		0xca07'04a6U, // eor x6, x5, x7, lsl #1
		0xea06'00dfU, // tst x6, x6 (ands xzr, x6, x6)
		0xd400'0001U, // svc #0
	};
	const auto register_logical = gadgets::Builder{}.build(
		base, register_logical_instructions);
	require(register_logical.complete);
	cpu = {};
	cpu.registers[0] = 5;
	cpu.registers[7] = 3;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(register_logical.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[5] == 5);
	require(cpu.registers[6] == 3);
	require(cpu.nzcv == 0);

	const std::array bitfield_instructions{
		0x1301'7c04U, // asr w4, w0, #1
		0xd348'5c25U, // ubfx x5, x1, #8, #16
		0x3318'1c46U, // bfi w6, w2, #8, #8
		0xd400'0001U, // svc #0
	};
	const auto bitfield = gadgets::Builder{}.build(base, bitfield_instructions);
	require(bitfield.complete);
	cpu = {};
	cpu.registers[0] = 0x8000'0000U;
	cpu.registers[1] = 0x0123'4567'89ab'cdefULL;
	cpu.registers[2] = 0xaa;
	cpu.registers[6] = 0x1122'3344U;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(bitfield.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[4] == 0xc000'0000U);
	require(cpu.registers[5] == 0xabcdU);
	require(cpu.registers[6] == 0x1122'aa44U);

	const std::array bit_reverse_instructions{
		0xdac0'0067U, // rbit x7, x3
		0xdac0'0468U, // rev16 x8, x3
		0xdac0'0869U, // rev32 x9, x3
		0xdac0'0c6aU, // rev x10, x3
		0xdac0'106bU, // clz x11, x3
		0xdac0'148cU, // cls x12, x4
		0x5ac0'00adU, // rbit w13, w5
		0x5ac0'04aeU, // rev16 w14, w5
		0x5ac0'08afU, // rev w15, w5
		0x5ac0'10b0U, // clz w16, w5
		0x5ac0'14b1U, // cls w17, w5
		0xd400'0001U, // svc #0
	};
	const auto bit_reverse = gadgets::Builder{}.build(
		base, bit_reverse_instructions);
	require(bit_reverse.complete);
	cpu = {};
	cpu.registers[3] = 0x0123'4567'89ab'cdefULL;
	cpu.registers[4] = 0xf000'0000'0000'0000ULL;
	cpu.registers[5] = 0x89ab'cdefU;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(bit_reverse.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[7] == 0xf7b3'd591'e6a2'c480ULL);
	require(cpu.registers[8] == 0x2301'6745'ab89'efcdULL);
	require(cpu.registers[9] == 0x6745'2301'efcd'ab89ULL);
	require(cpu.registers[10] == 0xefcd'ab89'6745'2301ULL);
	require(cpu.registers[11] == 7);
	require(cpu.registers[12] == 3);
	require(cpu.registers[13] == 0xf7b3'd591U);
	require(cpu.registers[14] == 0xab89'efcdU);
	require(cpu.registers[15] == 0xefcd'ab89U);
	require(cpu.registers[16] == 0);
	require(cpu.registers[17] == 0);

	const std::array simd_bitwise_instructions{
		0x4e22'1c20U, // and v0.16b, v1.16b, v2.16b
		0x0e65'1c83U, // bic v3.8b, v4.8b, v5.8b
		0x4ea8'1ce6U, // orr v6.16b, v7.16b, v8.16b
		0x0eeb'1d49U, // orn v9.8b, v10.8b, v11.8b
		0x6e2e'1dacU, // eor v12.16b, v13.16b, v14.16b
		0x2e71'1e0fU, // bsl v15.8b, v16.8b, v17.8b
		0x6eb4'1e72U, // bit v18.16b, v19.16b, v20.16b
		0x2ef7'1ed5U, // bif v21.8b, v22.8b, v23.8b
		0xd400'0001U, // svc #0
	};
	const auto simd_bitwise = gadgets::Builder{}.build(base, simd_bitwise_instructions);
	require(simd_bitwise.complete);
	cpu = {};
	cpu.vectors[1].bytes.fill(std::byte{0xf0});
	cpu.vectors[2].bytes.fill(std::byte{0x3c});
	cpu.vectors[4].bytes.fill(std::byte{0xf0});
	cpu.vectors[5].bytes.fill(std::byte{0x0f});
	cpu.vectors[7].bytes.fill(std::byte{0xf0});
	cpu.vectors[8].bytes.fill(std::byte{0x0f});
	cpu.vectors[11].bytes.fill(std::byte{0xf0});
	cpu.vectors[13].bytes.fill(std::byte{0xaa});
	cpu.vectors[14].bytes.fill(std::byte{0xff});
	cpu.vectors[15].bytes.fill(std::byte{0xf0});
	cpu.vectors[16].bytes.fill(std::byte{0xaa});
	cpu.vectors[17].bytes.fill(std::byte{0x55});
	cpu.vectors[18].bytes.fill(std::byte{0xf0});
	cpu.vectors[19].bytes.fill(std::byte{0xaa});
	cpu.vectors[20].bytes.fill(std::byte{0x0f});
	cpu.vectors[21].bytes.fill(std::byte{0xf0});
	cpu.vectors[22].bytes.fill(std::byte{0xaa});
	cpu.vectors[23].bytes.fill(std::byte{0x0f});
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(simd_bitwise.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	for (std::size_t index = 0; index < 16; ++index) {
		require(cpu.vectors[0].bytes[index] == std::byte{0x30});
		require(cpu.vectors[6].bytes[index] == std::byte{0xff});
		require(cpu.vectors[12].bytes[index] == std::byte{0x55});
		require(cpu.vectors[18].bytes[index] == std::byte{0xfa});
	}
	for (std::size_t index = 0; index < 8; ++index) {
		require(cpu.vectors[3].bytes[index] == std::byte{0xf0});
		require(cpu.vectors[9].bytes[index] == std::byte{0x0f});
		require(cpu.vectors[15].bytes[index] == std::byte{0xa5});
		require(cpu.vectors[21].bytes[index] == std::byte{0xa0});
	}
	for (std::size_t index = 8; index < 16; ++index) {
		require(cpu.vectors[3].bytes[index] == std::byte{});
		require(cpu.vectors[9].bytes[index] == std::byte{});
		require(cpu.vectors[15].bytes[index] == std::byte{});
		require(cpu.vectors[21].bytes[index] == std::byte{});
	}

	const std::array simd_move_instructions{
		0x4f00'041fU, // movi v31.4s, #0
		0x9e66'0042U, // fmov x2, d2
		0x9e67'0083U, // fmov d3, x4
		0x1e26'00c5U, // fmov w5, s6
		0x1e27'0107U, // fmov s7, w8
		0x9eae'0149U, // fmov x9, v10.d[1]
		0x9eaf'018bU, // fmov v11.d[1], x12
		0xd400'0001U, // svc #0
	};
	const auto simd_move = gadgets::Builder{}.build(base, simd_move_instructions);
	require(simd_move.complete);
	cpu = {};
	cpu.vectors[31].bytes.fill(std::byte{0xff});
	uml::backend::arm64::detail::set_vector_half(
		cpu.vectors[2], 0, 0x1122'3344'5566'7788ULL);
	uml::backend::arm64::detail::set_vector_half(
		cpu.vectors[6], 0, 0xffff'ffff'aabb'ccddULL);
	uml::backend::arm64::detail::set_vector_half(
		cpu.vectors[10], 1, 0x8877'6655'4433'2211ULL);
	uml::backend::arm64::detail::set_vector_half(
		cpu.vectors[11], 0, 0xfeed'face'cafe'beefULL);
	cpu.registers[4] = 0x0123'4567'89ab'cdefULL;
	cpu.registers[8] = 0xffff'ffff'7654'3210ULL;
	cpu.registers[12] = 0x1020'3040'5060'7080ULL;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(simd_move.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	for (const auto byte : cpu.vectors[31].bytes)
		require(byte == std::byte{});
	require(cpu.registers[2] == 0x1122'3344'5566'7788ULL);
	require(uml::backend::arm64::detail::vector_half(cpu.vectors[3], 0) ==
		0x0123'4567'89ab'cdefULL);
	require(uml::backend::arm64::detail::vector_half(cpu.vectors[3], 1) == 0);
	require(cpu.registers[5] == 0xaabb'ccddULL);
	require(uml::backend::arm64::detail::vector_half(cpu.vectors[7], 0) ==
		0x7654'3210ULL);
	require(uml::backend::arm64::detail::vector_half(cpu.vectors[7], 1) == 0);
	require(cpu.registers[9] == 0x8877'6655'4433'2211ULL);
	require(uml::backend::arm64::detail::vector_half(cpu.vectors[11], 0) ==
		0xfeed'face'cafe'beefULL);
	require(uml::backend::arm64::detail::vector_half(cpu.vectors[11], 1) ==
		0x1020'3040'5060'7080ULL);

	const std::array floating_select_instructions{
		0x1e24'cc62U, // fcsel s2, s3, s4, gt
		0x1e67'1cc5U, // fcsel d5, d6, d7, ne
		0xd400'0001U, // svc #0
	};
	const auto floating_select = gadgets::Builder{}.build(
		base, floating_select_instructions);
	require(floating_select.complete);
	cpu = {};
	cpu.nzcv = 0x4000'0000U; // Z=1: both GT and NE select the false source.
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[3], 0,
		std::bit_cast<std::uint32_t>(3.0F));
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[4], 0,
		std::bit_cast<std::uint32_t>(4.0F));
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[6], 0,
		std::bit_cast<std::uint64_t>(6.0));
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[7], 0,
		std::bit_cast<std::uint64_t>(7.0));
	cpu.vectors[2].bytes.fill(std::byte{0xaa});
	cpu.vectors[5].bytes.fill(std::byte{0xaa});
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(floating_select.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		uml::backend::arm64::detail::vector_half(cpu.vectors[2], 0))) == 4.0F);
	require(std::bit_cast<double>(
		uml::backend::arm64::detail::vector_half(cpu.vectors[5], 0)) == 7.0);
	for (std::size_t index = 4; index < 16; ++index)
		require(cpu.vectors[2].bytes[index] == std::byte{});
	for (std::size_t index = 8; index < 16; ++index)
		require(cpu.vectors[5].bytes[index] == std::byte{});

	const std::array floating_arithmetic_instructions{
		0x1e64'901eU, // fmov d30, #10.0
		0x1e60'c006U, // fabs d6, d0
		0x1e7f'28c7U, // fadd d7, d6, d31
		0x1e2a'1928U, // fdiv s8, s9, s10
		0xd400'0001U, // svc #0
	};
	const auto floating_arithmetic = gadgets::Builder{}.build(
		base, floating_arithmetic_instructions);
	require(floating_arithmetic.complete);
	cpu = {};
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[0], 0,
		std::bit_cast<std::uint64_t>(-3.0));
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[31], 0,
		std::bit_cast<std::uint64_t>(4.0));
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[9], 0,
		std::bit_cast<std::uint32_t>(9.0F));
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[10], 0,
		std::bit_cast<std::uint32_t>(3.0F));
	const auto saved_fpcr = read_fpcr();
	const auto saved_fpsr = read_fpsr();
	const auto caller_fpcr = saved_fpcr ^ (std::uint64_t{1} << 22);
	const auto caller_fpsr = saved_fpsr ^ 1U;
	write_fpcr(caller_fpcr);
	write_fpsr(caller_fpsr);
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(floating_arithmetic.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(read_fpcr() == caller_fpcr);
	require(read_fpsr() == caller_fpsr);
	write_fpcr(saved_fpcr);
	write_fpsr(saved_fpsr);
	require(std::bit_cast<double>(
		uml::backend::arm64::detail::vector_half(cpu.vectors[6], 0)) == 3.0);
	require(std::bit_cast<double>(
		uml::backend::arm64::detail::vector_half(cpu.vectors[7], 0)) == 7.0);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		uml::backend::arm64::detail::vector_half(cpu.vectors[8], 0))) == 3.0F);
	require(std::bit_cast<double>(
		uml::backend::arm64::detail::vector_half(cpu.vectors[30], 0)) == 10.0);
	for (const auto destination : {6U, 7U, 8U})
		for (std::size_t index = destination == 8 ? 4 : 8; index < 16; ++index)
			require(cpu.vectors[destination].bytes[index] == std::byte{});

	const std::array floating_flush_instructions{
		0x1e62'2820U, // fadd d0, d1, d2
		0xd400'0001U, // svc #0
	};
	const auto floating_flush = gadgets::Builder{}.build(
		base, floating_flush_instructions);
	require(floating_flush.complete);
	cpu = {};
	cpu.fpcr = 1U << 24; // FZ: flush denormal inputs and outputs to zero.
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[1], 0, 1);
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[2], 0, 0);
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(floating_flush.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(uml::backend::arm64::detail::vector_half(cpu.vectors[0], 0) == 0);

	const std::array floating_multiply_add_instructions{
		0x1f02'0c20U, // fmadd s0, s1, s2, s3
		0x1f06'9ca4U, // fmsub s4, s5, s6, s7
		0x1f6a'2d28U, // fnmadd d8, d9, d10, d11
		0x1f6e'bdacU, // fnmsub d12, d13, d14, d15
		0xd400'0001U, // svc #0
	};
	const auto floating_multiply_add = gadgets::Builder{}.build(
		base, floating_multiply_add_instructions);
	require(floating_multiply_add.complete);
	cpu = {};
	for (const auto index : {1U, 5U})
		uml::backend::arm64::detail::set_vector_half(cpu.vectors[index], 0,
			std::bit_cast<std::uint32_t>(2.0F));
	for (const auto index : {2U, 6U})
		uml::backend::arm64::detail::set_vector_half(cpu.vectors[index], 0,
			std::bit_cast<std::uint32_t>(3.0F));
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[3], 0,
		std::bit_cast<std::uint32_t>(4.0F));
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[7], 0,
		std::bit_cast<std::uint32_t>(20.0F));
	for (const auto index : {9U, 13U})
		uml::backend::arm64::detail::set_vector_half(cpu.vectors[index], 0,
			std::bit_cast<std::uint64_t>(2.0));
	for (const auto index : {10U, 14U})
		uml::backend::arm64::detail::set_vector_half(cpu.vectors[index], 0,
			std::bit_cast<std::uint64_t>(3.0));
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[11], 0,
		std::bit_cast<std::uint64_t>(4.0));
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[15], 0,
		std::bit_cast<std::uint64_t>(20.0));
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(floating_multiply_add.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		uml::backend::arm64::detail::vector_half(cpu.vectors[0], 0))) == 10.0F);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		uml::backend::arm64::detail::vector_half(cpu.vectors[4], 0))) == 14.0F);
	require(std::bit_cast<double>(
		uml::backend::arm64::detail::vector_half(cpu.vectors[8], 0)) == -10.0);
	require(std::bit_cast<double>(
		uml::backend::arm64::detail::vector_half(cpu.vectors[12], 0)) == -14.0);
	for (const auto destination : {0U, 4U, 8U, 12U})
		for (std::size_t index = destination < 8 ? 4 : 8; index < 16; ++index)
			require(cpu.vectors[destination].bytes[index] == std::byte{});

	const std::array simd_instructions{
		0x4e01'0c20U, // dup v0.16b, w1
		0xd400'0001U, // svc #0
	};
	const auto simd = gadgets::Builder{}.build(base, simd_instructions);
	require(simd.complete);
	cpu = {};
	cpu.registers[1] = 0xab;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(simd.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	for (const auto byte : cpu.vectors[0].bytes)
		require(byte == std::byte{0xab});

	const std::array scalar_shift_instructions{
		0x7f60'07ffU, // ushr d31, d31, #32
		0xd400'0001U, // svc #0
	};
	const auto scalar_shift = gadgets::Builder{}.build(
		base, scalar_shift_instructions);
	require(scalar_shift.complete);
	cpu = {};
	uml::backend::arm64::detail::set_vector_half(
		cpu.vectors[31], 0, 0xfedc'ba98'7654'3210ULL);
	uml::backend::arm64::detail::set_vector_half(
		cpu.vectors[31], 1, 0xffff'ffff'ffff'ffffULL);
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(scalar_shift.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(uml::backend::arm64::detail::vector_half(cpu.vectors[31], 0) ==
		0xfedc'ba98ULL);
	require(uml::backend::arm64::detail::vector_half(cpu.vectors[31], 1) == 0);

	const std::array shift_long_instructions{
		0x0f08'a7fdU, // sshll v29.8h, v31.8b, #0
		0xd400'0001U, // svc #0
	};
	const auto shift_long = gadgets::Builder{}.build(base, shift_long_instructions);
	require(shift_long.complete);
	cpu = {};
	const std::array source_bytes{
		std::byte{0x80}, std::byte{0x7f}, std::byte{0xff}, std::byte{0x00},
		std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
	};
	std::copy(source_bytes.begin(), source_bytes.end(),
		cpu.vectors[31].bytes.begin());
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(shift_long.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	const std::array expected_long{
		std::byte{0x80}, std::byte{0xff}, std::byte{0x7f}, std::byte{0x00},
		std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x00},
		std::byte{0x01}, std::byte{0x00}, std::byte{0x02}, std::byte{0x00},
		std::byte{0x03}, std::byte{0x00}, std::byte{0x04}, std::byte{0x00},
	};
	require(cpu.vectors[29].bytes == expected_long);

	const std::array conditional_select_instructions{
		0x9a85'0083U, // csel x3, x4, x5, eq
		0x1a88'14e6U, // csinc w6, w7, w8, ne
		0xda8b'4149U, // csinv x9, x10, x11, mi
		0xda8e'55acU, // csneg x12, x13, x14, pl
		0xd400'0001U, // svc #0
	};
	const auto conditional_select = gadgets::Builder{}.build(
		base, conditional_select_instructions);
	require(conditional_select.complete);
	cpu = {};
	cpu.nzcv = 0x4000'0000U; // Z=1, N=0.
	cpu.registers[4] = 0x4444;
	cpu.registers[5] = 0x5555;
	cpu.registers[7] = 0xffff'ffff'7777'7777ULL;
	cpu.registers[8] = 0xffff'ffff'ffff'fffeULL;
	cpu.registers[10] = 0xaaaa;
	cpu.registers[11] = 0x1111;
	cpu.registers[13] = 0xdddd;
	cpu.registers[14] = 0xeeee;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(conditional_select.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[3] == 0x4444);
	require(cpu.registers[6] == 0xffff'ffffU);
	require(cpu.registers[9] == ~std::uint64_t{0x1111});
	require(cpu.registers[12] == 0xdddd);

	const std::array conditional_negate_instructions{
		0xda8e'55acU, // csneg x12, x13, x14, pl
		0xd400'0001U, // svc #0
	};
	const auto conditional_negate = gadgets::Builder{}.build(
		base, conditional_negate_instructions);
	require(conditional_negate.complete);
	cpu = {};
	cpu.nzcv = 0x8000'0000U; // N=1, so PL is false.
	cpu.registers[13] = 5;
	cpu.registers[14] = 7;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(conditional_negate.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[12] == static_cast<std::uint64_t>(-7));

	/* Exercise both register/immediate forms, both widths, and every host
	 * condition specialization against the architectural implementation. */
	for (unsigned condition = 0; condition < 16; ++condition) {
		for (unsigned flags = 0; flags < 16; ++flags) {
			for (const bool subtract : {false, true}) {
				for (const bool immediate : {false, true}) {
					for (const bool wide : {false, true}) {
						const std::uint32_t instruction =
							(wide ? 0x8000'0000U : 0) |
							(subtract ? 0x4000'0000U : 0) |
							0x3a40'0000U |
							(immediate ? 0x0000'0800U : 0) |
							(condition << 12) | (7U << 16) |
							(3U << 5) | 5U;
						const std::array compare_instructions{
							instruction,
							0xd400'0001U, // svc #0
						};
						const auto compare = gadgets::Builder{}.build(
							base, compare_instructions);
						require(compare.complete);
						cpu = {};
						cpu.pc = base;
						cpu.nzcv = flags << 28;
						cpu.registers[3] = wide ?
							0x8000'0000'0000'0003ULL : 0x8000'0003U;
						cpu.registers[7] = wide ?
							0x7fff'ffff'ffff'fff9ULL : 0x7fff'fff9U;
						auto expected = cpu;
						if (uml::backend::arm64::detail::condition_holds(
						    expected.nzcv, condition)) {
							const auto mask = wide ? ~std::uint64_t{} :
								0xffff'ffffULL;
							const auto left = expected.registers[3] & mask;
							const auto right = (immediate ? 7U :
								expected.registers[7]) & mask;
							const auto result = (subtract ? left - right :
								left + right) & mask;
							uml::backend::arm64::detail::set_add_sub_flags(
								expected, left, right, result, wide, subtract);
						} else {
							expected.nzcv = (instruction & 15U) << 28;
						}
						tlb->prepare(0, darwin_uml_static_vm_changes(0));
						require(gadgets::execute(compare.program, cpu, *tlb) ==
							gadgets::Event::system_call);
						require(cpu.nzcv == expected.nzcv);
					}
				}
			}
		}
	}

	const std::array unsupported{0xffff'ffffU};
	const auto rejected = gadgets::Builder{}.build(base, unsupported);
	require(rejected.complete);
	cpu.pc = base;
	require(gadgets::execute(rejected.program, cpu, *tlb) ==
		gadgets::Event::fatal);

	const std::array undefined{0x0000'1234U}; // udf #0x1234
	const auto illegal = gadgets::Builder{}.build(base, undefined);
	require(illegal.complete);
	cpu.pc = base;
	require(gadgets::execute(illegal.program, cpu, *tlb) ==
		gadgets::Event::illegal_instruction);
	require(cpu.pc == base);

	const std::array decoded_instructions{
		0x9a02'0020U, // adc x0, x1, x2
		0xd400'0001U, // svc #0
	};
	const auto decoded = gadgets::Builder{}.build(base, decoded_instructions);
	require(decoded.complete);
	cpu = {};
	cpu.pc = base;
	cpu.registers[1] = 4;
	cpu.registers[2] = 5;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(decoded.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[0] == 9);

	const std::array branch_instructions{
		0xd280'0020U, // movz x0, #1
		0xb400'0040U, // cbz x0, +8 (not taken)
	};
	const auto branch = gadgets::Builder{}.build(base, branch_instructions);
	require(branch.complete);
	cpu = {};
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(branch.program, cpu, *tlb) ==
		gadgets::Event::continued);
	require(cpu.pc == base + 8);

	const std::array conditional_branch_instructions{
		0x5400'0040U, // b.eq +8
	};
	const auto conditional_branch = gadgets::Builder{}.build(
		base, conditional_branch_instructions);
	require(conditional_branch.complete);
	cpu = {};
	cpu.nzcv = 0x4000'0000U;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(conditional_branch.program, cpu, *tlb) ==
		gadgets::Event::continued);
	require(cpu.pc == base + 8);
	cpu.nzcv = 0;
	require(gadgets::execute(conditional_branch.program, cpu, *tlb) ==
		gadgets::Event::continued);
	require(cpu.pc == base + 4);

	/* The builder specializes every B.cond encoding to a fixed assembly
	 * gadget.  Compare every encoding and every NZCV combination with the
	 * architecture helper so none of the inverse or compound conditions can
	 * silently select the wrong edge.
	 */
	for (unsigned condition = 0; condition < 16; ++condition) {
		const std::array condition_instruction{
			0x5400'0040U | condition, // b.cond +8
		};
		const auto condition_program = gadgets::Builder{}.build(
			base, condition_instruction);
		require(condition_program.complete);
		for (unsigned flags = 0; flags < 16; ++flags) {
			cpu = {};
			cpu.nzcv = flags << 28;
			tlb->prepare(0, darwin_uml_static_vm_changes(0));
			require(gadgets::execute(
				condition_program.program, cpu, *tlb) ==
				gadgets::Event::continued);
			const bool taken = uml::backend::arm64::detail::condition_holds(
				cpu.nzcv, condition);
			require(cpu.pc == base + (taken ? 8 : 4));
		}
	}

	const std::array test_branch_instructions{
		0x3640'004bU, // tbz x11, #8, +8
	};
	const auto test_branch = gadgets::Builder{}.build(
		base, test_branch_instructions);
	require(test_branch.complete);
	cpu = {};
	cpu.registers[11] = 0x100;
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(test_branch.program, cpu, *tlb) ==
		gadgets::Event::continued);
	require(cpu.pc == base + 4);
	cpu.registers[11] = 0;
	require(gadgets::execute(test_branch.program, cpu, *tlb) ==
		gadgets::Event::continued);
	require(cpu.pc == base + 8);

	char path[] = "/private/tmp/uml-gadget-test.XXXXXX";
	const int fd = mkstemp(path);
	require(fd >= 0);
	require(unlink(path) == 0);
	require(ftruncate(fd, 8192) == 0);
	constexpr std::uint64_t guest_page = 0x1'0000;
	constexpr unsigned long generation = 19;
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(reinterpret_cast<void *>(guest_page), fd,
		0, 4096, PROT_READ | PROT_WRITE, 0, 1) == 0);
	std::array<std::byte, 64> structure_bytes{};
	for (std::size_t index = 0; index < structure_bytes.size(); ++index)
		structure_bytes[index] = static_cast<std::byte>(index * 7 + 3);
	require(pwrite(fd, structure_bytes.data(), structure_bytes.size(), 0x200) ==
		static_cast<ssize_t>(structure_bytes.size()));
	const std::array structure_instructions{
		0x4cdf'afe4U, // ld1 {v4.2d, v5.2d}, [sp], #32
		0xd400'0001U, // svc #0
	};
	const auto structure_program = gadgets::Builder{}.build(
		0x7f00, structure_instructions);
	require(structure_program.complete);
	auto structure_tlb = std::make_unique<gadgets::Tlb>();
	cpu = {};
	cpu.sp = guest_page + 0x200;
	structure_tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(structure_program.program, cpu, *structure_tlb) ==
		gadgets::Event::system_call);
	require(cpu.sp == guest_page + 0x220);
	require(std::memcmp(cpu.vectors[4].bytes.data(), structure_bytes.data(),
		16) == 0);
	require(std::memcmp(cpu.vectors[5].bytes.data(), structure_bytes.data() + 16,
		16) == 0);
	const std::uint64_t replicate_value = 0xfedc'ba98'7654'3210ULL;
	require(pwrite(fd, &replicate_value, sizeof(replicate_value), 0x280) ==
		static_cast<ssize_t>(sizeof(replicate_value)));
	const std::array replicate_instructions{
		0x4d40'cc1fU, // ld1r {v31.2d}, [x0]
		0xd400'0001U, // svc #0
	};
	const auto replicate_program = gadgets::Builder{}.build(
		0x7f80, replicate_instructions);
	require(replicate_program.complete);
	cpu = {};
	cpu.registers[0] = guest_page + 0x280;
	structure_tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(replicate_program.program, cpu, *structure_tlb) ==
		gadgets::Event::system_call);
	std::array<std::uint64_t, 2> replicated{};
	std::memcpy(replicated.data(), cpu.vectors[31].bytes.data(),
		sizeof(replicated));
	require(replicated[0] == replicate_value);
	require(replicated[1] == replicate_value);
	const std::uint64_t lane_value = 0x7654'3210'fedc'ba98ULL;
	require(pwrite(fd, &lane_value, sizeof(lane_value), 0x288) ==
		static_cast<ssize_t>(sizeof(lane_value)));
	const std::array lane_instructions{
		0x4d40'845fU, // ld1 {v31.d}[1], [x2]
		0xd400'0001U, // svc #0
	};
	const auto lane_program = gadgets::Builder{}.build(
		0x7fa0, lane_instructions);
	require(lane_program.complete);
	cpu = {};
	cpu.registers[2] = guest_page + 0x288;
	const std::uint64_t preserved_lane = 0x0123'4567'89ab'cdefULL;
	std::memcpy(cpu.vectors[31].bytes.data(), &preserved_lane,
		sizeof(preserved_lane));
	structure_tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(lane_program.program, cpu, *structure_tlb) ==
		gadgets::Event::system_call);
	std::array<std::uint64_t, 2> lane_result{};
	std::memcpy(lane_result.data(), cpu.vectors[31].bytes.data(),
		sizeof(lane_result));
	require(lane_result[0] == preserved_lane);
	require(lane_result[1] == lane_value);
	const std::array memory_instructions{
		0xd282'4680U, // movz x0, #0x1234
		0xd2a0'0022U, // movz x2, #1, lsl #16
		0xf900'0040U, // str x0, [x2]
		0xf940'0041U, // ldr x1, [x2]
		0xd400'0001U, // svc #0
	};
	const auto memory_program = gadgets::Builder{}.build(
		0x8000, memory_instructions);
	require(memory_program.complete);
	cpu = {};
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(memory_program.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[1] == 0x1234);
	std::uint64_t stored = 0;
	require(pread(fd, &stored, sizeof(stored), 0) == sizeof(stored));
	require(stored == 0x1234);

	const std::array subword_memory_instructions{
		0x3900'0440U, // strb w0, [x2, #1]
		0x3940'0441U, // ldrb w1, [x2, #1]
		0x3980'0443U, // ldrsb x3, [x2, #1]
		0x39c0'0444U, // ldrsb w4, [x2, #1]
		0x7900'0440U, // strh w0, [x2, #2]
		0x7940'0445U, // ldrh w5, [x2, #2]
		0x7980'0446U, // ldrsh x6, [x2, #2]
		0x79c0'0447U, // ldrsh w7, [x2, #2]
		0xb900'0440U, // str w0, [x2, #4]
		0xb980'0448U, // ldrsw x8, [x2, #4]
		0xd400'0001U, // svc #0
	};
	const auto subword_memory = gadgets::Builder{}.build(
		0x8200, subword_memory_instructions);
	require(subword_memory.complete);
	cpu = {};
	cpu.registers[0] = 0xffff'ffff'8000'8080ULL;
	cpu.registers[2] = guest_page;
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(subword_memory.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[1] == 0x80);
	require(cpu.registers[3] == 0xffff'ffff'ffff'ff80ULL);
	require(cpu.registers[4] == 0xffff'ff80U);
	require(cpu.registers[5] == 0x8080U);
	require(cpu.registers[6] == 0xffff'ffff'ffff'8080ULL);
	require(cpu.registers[7] == 0xffff'8080U);
	require(cpu.registers[8] == 0xffff'ffff'8000'8080ULL);

	const std::array vector_memory_instructions{
		0xfd00'0440U, // str d0, [x2, #8]
		0xfd40'0441U, // ldr d1, [x2, #8]
		0xd400'0001U, // svc #0
	};
	const auto vector_memory = gadgets::Builder{}.build(
		0x8300, vector_memory_instructions);
	require(vector_memory.complete);
	cpu = {};
	cpu.registers[2] = guest_page;
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[0], 0,
		0x0123'4567'89ab'cdefULL);
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[0], 1,
		0xfedc'ba98'7654'3210ULL);
	cpu.vectors[1].bytes.fill(std::byte{0xaa});
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(vector_memory.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(uml::backend::arm64::detail::vector_half(
		cpu.vectors[1], 0) == 0x0123'4567'89ab'cdefULL);
	require(uml::backend::arm64::detail::vector_half(cpu.vectors[1], 1) == 0);

	const std::array vector_other_instructions{
		0xfc1f'8c40U, // str d0, [x2, #-8]!
		0xfc40'8441U, // ldr d1, [x2], #8
		0xd400'0001U, // svc #0
	};
	const auto vector_other = gadgets::Builder{}.build(
		0x8340, vector_other_instructions);
	require(vector_other.complete);
	cpu = {};
	cpu.registers[2] = guest_page + 0x20;
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[0], 0,
		0x8877'6655'4433'2211ULL);
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[0], 1,
		~std::uint64_t{0});
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(vector_other.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[2] == guest_page + 0x20);
	require(uml::backend::arm64::detail::vector_half(
		cpu.vectors[1], 0) == 0x8877'6655'4433'2211ULL);
	require(uml::backend::arm64::detail::vector_half(cpu.vectors[1], 1) == 0);

	const std::array vector_pair_instructions{
		0x6dbf'07e0U, // stp d0, d1, [sp, #-16]!
		0x6cc1'0fe2U, // ldp d2, d3, [sp], #16
		0xd400'0001U, // svc #0
	};
	const auto vector_pair = gadgets::Builder{}.build(
		0x8380, vector_pair_instructions);
	require(vector_pair.complete);
	cpu = {};
	cpu.sp = guest_page + 0x100;
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[0], 0,
		0x0123'4567'89ab'cdefULL);
	uml::backend::arm64::detail::set_vector_half(cpu.vectors[1], 0,
		0xfedc'ba98'7654'3210ULL);
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(vector_pair.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.sp == guest_page + 0x100);
	require(uml::backend::arm64::detail::vector_half(
		cpu.vectors[2], 0) == 0x0123'4567'89ab'cdefULL);
	require(uml::backend::arm64::detail::vector_half(
		cpu.vectors[3], 0) == 0xfedc'ba98'7654'3210ULL);
	require(uml::backend::arm64::detail::vector_half(cpu.vectors[2], 1) == 0);
	require(uml::backend::arm64::detail::vector_half(cpu.vectors[3], 1) == 0);

	/* A direct 64-bit gadget must not access past the translated page.  The
	 * cross-page slow path validates the second page before changing either.
	 */
	const std::array cross_page_instructions{
		0xf900'0040U, // str x0, [x2]
		0xf940'0045U, // ldr x5, [x2]
		0xd400'0001U, // svc #0
	};
	const auto cross_page_program = gadgets::Builder{}.build(
		0x8400, cross_page_instructions);
	require(cross_page_program.complete);
	constexpr std::uint64_t crossing_address = guest_page + 4091;
	const std::uint64_t crossing_value = 0x8877'6655'4433'2211ULL;
	std::array<std::byte, 8> zero_bytes{};
	require(pwrite(fd, zero_bytes.data(), zero_bytes.size(), 4091) ==
		static_cast<ssize_t>(zero_bytes.size()));
	cpu = {};
	cpu.registers[0] = crossing_value;
	cpu.registers[2] = crossing_address;
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(cross_page_program.program, cpu, *tlb) ==
		gadgets::Event::memory_fault);
	require(cpu.pc == 0x8400);
	std::array<std::byte, 8> crossing_bytes{};
	require(pread(fd, crossing_bytes.data(), crossing_bytes.size(), 4091) ==
		static_cast<ssize_t>(crossing_bytes.size()));
	require(crossing_bytes == zero_bytes);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(guest_page + 4096), fd, 4096, 4096,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	cpu = {};
	cpu.registers[0] = crossing_value;
	cpu.registers[2] = crossing_address;
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(cross_page_program.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[5] == crossing_value);
	std::uint64_t crossing_stored = 0;
	require(pread(fd, &crossing_stored, sizeof(crossing_stored), 4091) ==
		static_cast<ssize_t>(sizeof(crossing_stored)));
	require(crossing_stored == crossing_value);

	const std::array<std::int32_t, 2> signed_pair{
		-0x1234'567, 0x2345'678,
	};
	require(pwrite(fd, signed_pair.data(), sizeof(signed_pair), 20) ==
		static_cast<ssize_t>(sizeof(signed_pair)));
	const std::array signed_pair_instructions{
		0x6942'8803U, // ldpsw x3, x2, [x0, #20]
		0xd400'0001U, // svc #0
	};
	const auto signed_pair_program = gadgets::Builder{}.build(
		0x8700, signed_pair_instructions);
	require(signed_pair_program.complete);
	cpu = {};
	cpu.registers[0] = guest_page;
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(signed_pair_program.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[3] == static_cast<std::uint64_t>(
		static_cast<std::int64_t>(signed_pair[0])));
	require(cpu.registers[2] == static_cast<std::uint64_t>(
		static_cast<std::int64_t>(signed_pair[1])));

	const std::array pair_instructions{
		0xa9bf'07e0U, // stp x0, x1, [sp, #-16]!
		0xa8c1'13e3U, // ldp x3, x4, [sp], #16
		0xd400'0001U, // svc #0
	};
	const auto pair_program = gadgets::Builder{}.build(
		0x8800, pair_instructions);
	require(pair_program.complete);
	cpu = {};
	cpu.sp = guest_page + 0x100;
	cpu.registers[0] = 0x0123'4567'89ab'cdefULL;
	cpu.registers[1] = 0xfedc'ba98'7654'3210ULL;
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(pair_program.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.sp == guest_page + 0x100);
	require(cpu.registers[3] == 0x0123'4567'89ab'cdefULL);
	require(cpu.registers[4] == 0xfedc'ba98'7654'3210ULL);
	std::array<std::uint64_t, 2> stored_pair{};
	require(pread(fd, stored_pair.data(), sizeof(stored_pair), 0xf0) ==
		static_cast<ssize_t>(sizeof(stored_pair)));
	require(stored_pair[0] == 0x0123'4567'89ab'cdefULL);
	require(stored_pair[1] == 0xfedc'ba98'7654'3210ULL);

	/* Guest pages 16 and 8209 hash to the same 8,192-entry TLB slot. Loading
	 * A, then B, then A again must miss on the final load instead of reusing
	 * B's host-base field under A's stale read tag.
	 */
	constexpr std::uint64_t collision_page = 0x2011'000;
	const std::uint64_t first_value = 0x1111'2222'3333'4444ULL;
	const std::uint64_t collision_value = 0xaaaa'bbbb'cccc'ddddULL;
	require(pwrite(fd, &first_value, sizeof(first_value), 0) ==
		static_cast<ssize_t>(sizeof(first_value)));
	require(pwrite(fd, &collision_value, sizeof(collision_value), 4096) ==
		static_cast<ssize_t>(sizeof(collision_value)));
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(collision_page), fd, 4096, 4096,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	const std::array collision_instructions{
		0xf940'0040U, // ldr x0, [x2]
		0xf940'0061U, // ldr x1, [x3]
		0xf940'0044U, // ldr x4, [x2]
		0xd400'0001U, // svc #0
	};
	const auto collision_program = gadgets::Builder{}.build(
		0x9000, collision_instructions);
	require(collision_program.complete);
	cpu = {};
	cpu.registers[2] = guest_page;
	cpu.registers[3] = collision_page;
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(collision_program.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.registers[0] == first_value);
	require(cpu.registers[1] == collision_value);
	require(cpu.registers[4] == first_value);

	/* Read and write translations have independent host bases.  Preserve a
	 * read translation when a write to a colliding page fills the other half
	 * of the same TLB entry.
	 */
	auto independent_tlb = std::make_unique<gadgets::Tlb>();
	independent_tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	auto *read_translation = independent_tlb->translate(
		guest_page, gadgets::Access::read);
	auto *write_translation = independent_tlb->translate(
		collision_page, gadgets::Access::write);
	require(read_translation != nullptr);
	require(write_translation != nullptr);
	constexpr std::size_t collision_slot = 16;
	require(independent_tlb->entries[collision_slot].read_page == guest_page);
	require(independent_tlb->entries[collision_slot].write_page ==
		collision_page);
	require(independent_tlb->translate(guest_page, gadgets::Access::read) ==
		read_translation);
	require(independent_tlb->translate(
		collision_page, gadgets::Access::write) == write_translation);

	/* Tlb::translate() is also the slow path used by decoded instructions
	 * such as LDXR/STXR.  Once populated, it must use the same cached host
	 * base as the assembly gadgets until prepare() observes a mapping change.
	 */
	constexpr std::uint64_t cached_page = 0x3000'000;
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(cached_page), fd, 0, 4096,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	auto *cached_first = tlb->translate(cached_page, gadgets::Access::read);
	require(cached_first != nullptr);
	require(*reinterpret_cast<std::uint64_t *>(cached_first) == first_value);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(cached_page), fd, 4096, 4096,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	require(tlb->translate(cached_page, gadgets::Access::read) == cached_first);
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	auto *cached_second = tlb->translate(cached_page, gadgets::Access::read);
	require(cached_second != nullptr);
	require(cached_second != cached_first);
	require(*reinterpret_cast<std::uint64_t *>(cached_second) == collision_value);
	require(darwin_uml_static_vm_destroy(generation) == 0);
	require(close(fd) == 0);
	return 0;
}
