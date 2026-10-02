// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_guest.h"
#include "uml_darwin_cpu.h"
#include "uml_darwin_static_mmu.h"
#include "jit_test_support.hpp"
#include "scratch_backing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <string_view>

namespace {

using uml::emulator::tests::JitMapping;
using uml::testing::ScratchBacking;

constexpr std::string_view backing_name = "uml-static-boundary.XXXXXX";

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

[[nodiscard]] constexpr std::uint32_t pstate(
	unsigned immediate, unsigned op2)
{
	return 0xd500'401fU | (3U << 16) |
		((immediate & 15U) << 8) | ((op2 & 7U) << 5);
}

int enter(darwin_uml_guest_registers &registers,
	darwin_uml_guest_fp_registers &fp, unsigned long generation)
{
	unsigned long tls = 0x1234'5678UL;
	return darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
}

void fpmr_is_not_guest_state()
{
	/* The virtual CPU implements no FEAT_FP8, so an arm64 guest ignores the
	 * bridge's FPMR slot on entry and reports it as zero. */
	constexpr unsigned long generation = 200;
	constexpr std::uint64_t base = 0x50'0000;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	registers.fpmr = 0x0000'003f'ff7f'c1ffULL;

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 4);
	require(registers.fpmr == 0);
}

void pstate_survives_boundary()
{
	/* DIT is guest state. The virtual CPU implements neither FEAT_SSBS nor
	 * FEAT_SME: MSR SSBS and SMSTART are undefined, the kernel's
	 * PSTATE.SSBS passes through untouched, and SVCR comes back zero. */
	constexpr unsigned long generation = 201;
	constexpr std::uint64_t base = 0x51'0000;
	constexpr std::uint32_t pstate_ssbs = 0x0000'1000U;
	constexpr std::uint32_t pstate_dit = 0x0100'0000U;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, pstate(1, 2));  // msr dit, #1
	backing.put32(4, 0xd503'201fU);  // nop
	backing.put32(8, 0xd400'0001U);  // svc #0
	backing.put32(12, pstate(0, 1)); // msr ssbs, #0
	backing.put32(16, pstate(7, 3)); // smstart
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	registers.pstate = pstate_ssbs;
	registers.svcr = 3;

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 12);
	require((registers.pstate & (pstate_dit | pstate_ssbs)) ==
		(pstate_dit | pstate_ssbs));
	require(registers.svcr == 0);
	require(enter(registers, fp, generation) ==
		DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION);
	require(registers.pc == base + 12);
	require((registers.pstate & pstate_ssbs) == pstate_ssbs);
	registers.pc = base + 16;
	require(enter(registers, fp, generation) ==
		DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION);
	require(registers.pc == base + 16);
	require(registers.svcr == 0);
}

void exclusive_monitor_is_cleared_at_boundary()
{
	constexpr unsigned long generation = 202;
	constexpr std::uint64_t base = 0x52'0000;
	constexpr std::uint64_t data = base + 0x800;
	constexpr std::uint64_t original = 0x1122'3344'5566'7788ULL;
	constexpr std::uint64_t replacement = 0x8877'6655'4433'2211ULL;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0xc85f'7c22U);  // ldxr x2, [x1]
	backing.put32(4, 0xd400'0001U);  // svc #0
	backing.put32(8, 0xc805'fc23U);  // stlxr w5, x3, [x1]
	backing.put32(12, 0xd400'0001U); // svc #0
	backing.put64(0x800, original);
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	registers.x[1] = data;
	registers.x[3] = replacement;

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[2] == original);
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[5] == 1);
	require(backing.get64(0x800) == original);
}

void deferred_fp_source_is_refreshed_at_boundary()
{
	constexpr unsigned long generation = 203;
	constexpr std::uint64_t base = 0x53'0000;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0x4f00'041fU); // movi v31.4s, #0
	backing.put32(4, 0xd400'0001U); // svc #0
	backing.put32(8, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	std::memset(fp.vectors[31], 0xff, sizeof(fp.vectors[31]));

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	for (const auto byte : fp.vectors[31])
		require(byte == 0);
	std::memset(fp.vectors[31], 0x5a, sizeof(fp.vectors[31]));
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	for (const auto byte : fp.vectors[31])
		require(byte == 0x5a);
}

void deferred_fp_source_is_materialized_on_linked_target()
{
	constexpr unsigned long generation = 204;
	constexpr std::uint64_t base = 0x54'0000;
	constexpr std::uint64_t first_value = 0x0123'4567'89ab'cdefULL;
	constexpr std::uint64_t second_value = 0xfedc'ba98'7654'3210ULL;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0x1400'0004U);  // b base + 16
	backing.put32(16, 0x9e66'0000U); // fmov x0, d0
	backing.put32(20, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};

	/* Translate and activate the FP target before its integer predecessor. */
	registers.pc = base + 16;
	std::memcpy(fp.vectors[0], &first_value, sizeof(first_value));
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == first_value);

	/* A new entry defers a different kernel frame. The cached predecessor now
	 * chains through the FP prefix and must observe this new source. */
	registers.pc = base;
	std::memcpy(fp.vectors[0], &second_value, sizeof(second_value));
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == second_value);
	require(registers.pc == base + 24);
}

} // namespace

int main()
{
	fpmr_is_not_guest_state();
	pstate_survives_boundary();
	exclusive_monitor_is_cleared_at_boundary();
	deferred_fp_source_is_refreshed_at_boundary();
	deferred_fp_source_is_materialized_on_linked_target();
}
