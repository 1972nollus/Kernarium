// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_guest.h"
#include "uml_darwin_cpu.h"
#include "uml_darwin_static_mmu.h"
#include "jit_test_support.hpp"
#include "scratch_backing.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace {

using uml::emulator::tests::JitMapping;
using uml::testing::ScratchBacking;

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

int enter(darwin_uml_guest_registers &registers,
	darwin_uml_guest_fp_registers &fp, unsigned long generation)
{
	unsigned long tls = 0;
	return darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
}

void translation_window_crosses_a_page()
{
	constexpr unsigned long generation = 210;
	constexpr std::uint64_t base = 0x60'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	ScratchBacking backing{"uml-static-translation.XXXXXX", 2 * page};
	backing.put32(page - 4, 0xd280'0540U); // mov x0, #42
	backing.put32(page, 0xd400'0001U);     // svc #0
	JitMapping mapping{generation, base, backing, 2};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base + page - 4;

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == 42);
	require(registers.pc == base + page + 4);
}

void translation_window_preserves_a_second_page_fault()
{
	constexpr unsigned long generation = 211;
	constexpr std::uint64_t base = 0x61'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	ScratchBacking backing{"uml-static-translation.XXXXXX", page};
	backing.put32(page - 4, 0xd280'0540U); // mov x0, #42
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base + page - 4;

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_FAULT);
	require(registers.x[0] == 42);
	require(registers.pc == base + page);
	require(registers.fault.address == base + page);
	require(registers.fault.error_code == DARWIN_UML_FAULT_EXECUTE);
}

} // namespace

int main()
{
	translation_window_crosses_a_page();
	translation_window_preserves_a_second_page_fault();
}
