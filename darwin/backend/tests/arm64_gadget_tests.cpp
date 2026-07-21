// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/detail/instruction_helpers.hpp"

#include <array>
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

int main()
{
	constexpr std::uint64_t base = 0x4000;
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
