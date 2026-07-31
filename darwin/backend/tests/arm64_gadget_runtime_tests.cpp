// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <mach/mach.h>
#include <memory>
#include <source_location>
#include <sys/mman.h>
#include <unistd.h>

namespace gadgets = uml::backend::arm64::gadgets;

namespace {

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

void require_data_only(const gadgets::Program &program)
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

void data_only_stream_and_execution_boundary()
{
	constexpr std::uint64_t base = 0x4000;
	constexpr std::array instructions{
		0xd503'201fU, // nop
		0xd400'0001U, // svc #0
	};
	const auto built = gadgets::Builder{}.build(base, instructions);
	require(built.complete);
	require(built.program.instruction_count() == instructions.size());
	require(!built.program.words().empty());
	require_data_only(built.program);

	uml::backend::arm64::CpuState cpu{};
	cpu.pc = base;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.pc == base + instructions.size() * sizeof(std::uint32_t));
}

void code_synchronization_is_a_precise_execution_boundary()
{
	char path[] = "/private/tmp/uml-gadget-code-sync-test.XXXXXX";
	const int fd = mkstemp(path);
	require(fd >= 0);
	require(unlink(path) == 0);
	require(ftruncate(fd, DARWIN_UML_GUEST_PAGE_SIZE) == 0);

	constexpr unsigned long generation = 18;
	constexpr std::uint64_t guest_page = 0xf'0000;
	constexpr std::uint64_t code_pc = 0x8200;
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(guest_page), fd, 0,
		DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE | PROT_EXEC, 0, 1) == 0);

	constexpr std::array instructions{
		0xd50b'7522U, // ic ivau, x2
		0x9100'0463U, // add x3, x3, #1
	};
	const auto built = gadgets::Builder{}.build(code_pc, instructions);
	require(built.complete);
	require(built.program.instruction_count() == 1);
	require_data_only(built.program);

	uml::backend::arm64::CpuState cpu{};
	cpu.pc = code_pc;
	cpu.registers[2] = guest_page;
	cpu.registers[3] = 7;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(generation, darwin_uml_static_vm_changes(generation));
	const auto before = darwin_uml_static_vm_code_changes(generation);
	std::size_t executed = 0;
	require(gadgets::execute(built.program, cpu, *tlb, executed) ==
		gadgets::Event::continued);
	require(executed == 1);
	require(cpu.pc == code_pc + sizeof(std::uint32_t));
	require(cpu.registers[3] == 7);
	require(darwin_uml_static_vm_code_changes(generation) == before + 1);

	require(darwin_uml_static_vm_destroy(generation) == 0);
	require(close(fd) == 0);
}

void mapped_memory_runtime_boundaries()
{
	char path[] = "/private/tmp/uml-gadget-runtime-test.XXXXXX";
	const int fd = mkstemp(path);
	require(fd >= 0);
	require(unlink(path) == 0);
	require(ftruncate(fd, 2 * DARWIN_UML_GUEST_PAGE_SIZE) == 0);

	constexpr std::uint64_t guest_page = 0x1'0000;
	constexpr unsigned long generation = 19;
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(reinterpret_cast<void *>(guest_page), fd,
		0, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);

	auto tlb = std::make_unique<gadgets::Tlb>();

	/*
	 * A direct 64-bit entry must not access beyond its translated page.
	 * The cross-page slow path validates the second page before changing
	 * either page, and resumes the same data-only program once it is mapped.
	 */
	constexpr std::array cross_page_instructions{
		0xf900'0040U, // str x0, [x2]
		0xf940'0045U, // ldr x5, [x2]
		0xd400'0001U, // svc #0
	};
	const auto cross_page_program = gadgets::Builder{}.build(
		0x8400, cross_page_instructions);
	require(cross_page_program.complete);
	constexpr auto crossing_offset = DARWIN_UML_GUEST_PAGE_SIZE - 5;
	constexpr std::uint64_t crossing_address = guest_page + crossing_offset;
	constexpr std::uint64_t crossing_value =
		0x8877'6655'4433'2211ULL;
	std::array<std::byte, 8> zero_bytes{};
	require(pwrite(fd, zero_bytes.data(), zero_bytes.size(), crossing_offset) ==
		static_cast<ssize_t>(zero_bytes.size()));
	uml::backend::arm64::CpuState cpu{};
	cpu.registers[0] = crossing_value;
	cpu.registers[2] = crossing_address;
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(cross_page_program.program, cpu, *tlb) ==
		gadgets::Event::memory_fault);
	require(cpu.pc == 0x8400);
	std::array<std::byte, 8> crossing_bytes{};
	require(pread(fd, crossing_bytes.data(), crossing_bytes.size(),
		crossing_offset) ==
		static_cast<ssize_t>(crossing_bytes.size()));
	require(crossing_bytes == zero_bytes);

	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(
			guest_page + DARWIN_UML_GUEST_PAGE_SIZE),
		fd, DARWIN_UML_GUEST_PAGE_SIZE, DARWIN_UML_GUEST_PAGE_SIZE,
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
	require(pread(fd, &crossing_stored, sizeof(crossing_stored),
		crossing_offset) ==
		static_cast<ssize_t>(sizeof(crossing_stored)));
	require(crossing_stored == crossing_value);

	/*
	 * Select two guest pages in the same build-specific set. Loading A, B,
	 * then A again exercises replacement without confusing their host bases.
	 */
	constexpr std::uint64_t collision_page =
		guest_page + (
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
			gadgets::tlb_set_count
#else
			gadgets::tlb_set_count + 1
#endif
		) *
			DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint64_t first_value = 0x1111'2222'3333'4444ULL;
	constexpr std::uint64_t collision_value =
		0xaaaa'bbbb'cccc'ddddULL;
	require(pwrite(fd, &first_value, sizeof(first_value), 0) ==
		static_cast<ssize_t>(sizeof(first_value)));
	require(pwrite(fd, &collision_value, sizeof(collision_value),
		DARWIN_UML_GUEST_PAGE_SIZE) ==
		static_cast<ssize_t>(sizeof(collision_value)));
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(collision_page), fd,
		DARWIN_UML_GUEST_PAGE_SIZE, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	constexpr std::array collision_instructions{
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
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
	constexpr auto collision_set =
		((guest_page >> DARWIN_UML_GUEST_PAGE_SHIFT) &
			(gadgets::tlb_set_count - 1)) *
		gadgets::tlb_way_count;
	require(tlb->entries[collision_set].read_page == guest_page);
	require(tlb->entries[collision_set + 1].read_page == collision_page);
#endif

	/*
	 * Read and write translations retain independent host bases even when
	 * their guest pages collide in the address cache.
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
	constexpr auto guest_page_number =
		guest_page >> DARWIN_UML_GUEST_PAGE_SHIFT;
	constexpr std::size_t collision_slot =
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
		(guest_page_number & (gadgets::tlb_set_count - 1)) *
		gadgets::tlb_way_count;
#else
		(guest_page_number ^
			(guest_page_number >> gadgets::tlb_index_bits)) &
		(gadgets::tlb_set_count - 1);
#endif
	require(independent_tlb->entries[collision_slot].read_page == guest_page);
	require(independent_tlb->entries[collision_slot].write_page ==
		collision_page);
	require(independent_tlb->translate(guest_page, gadgets::Access::read) ==
		read_translation);
	require(independent_tlb->translate(
		collision_page, gadgets::Access::write) == write_translation);

	/*
	 * A populated slow-path translation remains stable until prepare()
	 * observes the mapping change, then resolves the replacement host page.
	 */
	constexpr std::uint64_t cached_page = 0x3000'000;
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(cached_page), fd, 0,
		DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	auto *cached_first = tlb->translate(cached_page, gadgets::Access::read);
	require(cached_first != nullptr);
	require(*reinterpret_cast<std::uint64_t *>(cached_first) == first_value);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(cached_page), fd,
		DARWIN_UML_GUEST_PAGE_SIZE, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	require(tlb->translate(cached_page, gadgets::Access::read) == cached_first);
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	auto *cached_second = tlb->translate(cached_page, gadgets::Access::read);
	require(cached_second != nullptr);
	require(cached_second != cached_first);
	require(*reinterpret_cast<std::uint64_t *>(cached_second) ==
		collision_value);

	require(darwin_uml_static_vm_destroy(generation) == 0);
	require(close(fd) == 0);
}

} // namespace

int main()
{
	data_only_stream_and_execution_boundary();
	code_synchronization_is_a_precise_execution_boundary();
	mapped_memory_runtime_boundaries();
	return 0;
}
