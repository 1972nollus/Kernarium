// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>
#include <vector>

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

class NullMemory final : public arm64::GuestMemory {
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

void require(bool condition,
	     const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

constexpr std::uint64_t base = 0x2'0000;

[[nodiscard]] arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0xfedc'ba98'7654'3210ULL ^ index;
	for (unsigned index = 0; index < cpu.vectors.size(); ++index)
		for (unsigned byte = 0; byte < cpu.vectors[index].bytes.size(); ++byte)
			cpu.vectors[index].bytes[byte] = static_cast<std::byte>(
				(index * 17U + byte * 29U) & 0xffU);
	cpu.sp = 0x1234'5678'9abc'def0ULL;
	cpu.nzcv = 0x9000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x1020'3040'5060'7080ULL;
	cpu.exclusive_address = 0x1111'2222'3333'4440ULL;
	cpu.exclusive_key = 0x5555'6666U;
	cpu.exclusive_value = 0x7777'8888'9999'aaaaULL;
	cpu.exclusive_value2 = 0xbbbb'cccc'dddd'eeeeULL;
	cpu.exclusive_version = 0x1234'5678U;
	cpu.exclusive_size = 8;
	cpu.deferred_fpcr = 0x0100'0000U;
	cpu.deferred_fpsr = 0x0200'0000U;
	cpu.fp_dirty = true;
	return cpu;
}

void require_unchanged(const arm64::CpuState &actual,
		       const arm64::CpuState &initial, std::uint64_t pc)
{
	require(actual.registers == initial.registers);
	for (unsigned index = 0; index < actual.vectors.size(); ++index)
		require(actual.vectors[index].bytes == initial.vectors[index].bytes);
	require(actual.sp == initial.sp);
	require(actual.pc == pc);
	require(actual.nzcv == initial.nzcv);
	require(actual.fpcr == initial.fpcr);
	require(actual.fpsr == initial.fpsr);
	require(actual.tls == initial.tls);
	require(actual.exclusive_address == initial.exclusive_address);
	require(actual.exclusive_key == initial.exclusive_key);
	require(actual.exclusive_value == initial.exclusive_value);
	require(actual.exclusive_value2 == initial.exclusive_value2);
	require(actual.exclusive_version == initial.exclusive_version);
	require(actual.exclusive_size == initial.exclusive_size);
	require(actual.deferred_vectors == initial.deferred_vectors);
	require(actual.deferred_fpcr == initial.deferred_fpcr);
	require(actual.deferred_fpsr == initial.deferred_fpsr);
	require(actual.fp_loaded == initial.fp_loaded);
	require(actual.fp_dirty == initial.fp_dirty);
}

void check(std::uint32_t instruction)
{
	NullMemory memory;
	const auto initial = state();
	auto interpreted = initial;
	interpreted.pc = base;
	require(arm64::Dispatcher{}.execute(
		interpreted, memory, instruction).reason ==
		arm64::StopReason::continued);
	require_unchanged(interpreted, initial, base + 4);

	const std::array stream{instruction, 0xd400'0001U};
	const auto built = gadgets::Builder{}.build(base, stream);
	require(built.complete);
	auto gadget = initial;
	gadget.pc = base;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::system_call);
	require_unchanged(gadget, initial, base + 8);
}

[[nodiscard]] constexpr std::uint32_t hinte_instruction(unsigned immediate)
{
	return 0xd500'2000U |
		(((immediate >> 15) & 1U) << 21) |
		(((immediate >> 12) & 7U) << 16) |
		(((immediate >> 5) & 0x7fU) << 5) |
		(immediate & 0x1fU);
}

[[nodiscard]] constexpr bool is_primary_hint(unsigned immediate)
{
	return immediate >= 12'319U && immediate <= 16'383U &&
		((immediate - 12'319U) % 32U) == 0;
}

[[nodiscard]] constexpr bool is_allocated_primary_hint(unsigned immediate)
{
	if (immediate <= 7U || (immediate >= 16U && immediate <= 20U) ||
	    (immediate >= 24U && immediate <= 32U) || immediate == 34U ||
	    immediate == 36U || (immediate >= 38U && immediate <= 40U) ||
	    (immediate >= 48U && immediate <= 52U))
		return true;
	return immediate == 8U || immediate == 10U || immediate == 12U ||
		immediate == 14U || immediate == 22U;
}

void primary_hints()
{
	/* NOP, YIELD, WFE, WFI, DGH, and CSDB. Other allocated HINT values are
	 * covered below or by their stateful instruction-family tests.
	 */
	for (const unsigned immediate : {0U, 1U, 2U, 3U, 6U, 20U})
		check(0xd503'201fU | (immediate << 5));

	/* Direct host-backed performance/security hints must not collapse onto
	 * the ordinary NOP gadget in production.
	 */
	const auto nop = gadgets::Builder{}.build(
		base, std::array{0xd503'201fU});
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	(void)nop;
#endif
	for (const unsigned immediate : {1U, 6U, 20U, 22U}) {
		const auto specialized = gadgets::Builder{}.build(
			base, std::array{0xd503'201fU | (immediate << 5)});
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
		require(specialized.program.words().front() !=
			nop.program.words().front());
#else
		(void)specialized;
#endif
	}

	/* Every currently unallocated encoding in the 7-bit HINT space must
	 * retain its architecturally required NOP behavior. Keep allocated pointer
	 * authentication encodings for their dedicated tests.
	 */
	for (unsigned immediate = 0; immediate < 128U; ++immediate)
		if (!is_allocated_primary_hint(immediate))
			check(0xd503'201fU | (immediate << 5));
}

void stateless_architectural_aliases()
{
	/* SEV/SEVL are unobservable while guest WFE is permitted to complete
	 * spuriously. Optional subsystem hints take their specified feature-absent
	 * NOP behavior; CHKFEAT therefore reports no enabled GCS bits. BTI executes
	 * outside guarded guest memory in this emulator.
	 */
	for (const unsigned immediate : {4U, 5U, 16U, 17U, 18U, 19U, 22U,
		32U, 34U, 36U, 38U, 40U, 48U, 49U, 50U, 51U, 52U})
		check(0xd503'201fU | (immediate << 5));
}

void wait_with_timeout()
{
	for (unsigned source = 0; source < 32; ++source) {
		check(0xd503'1000U | source); // WFET
		check(0xd503'1020U | source); // WFIT
	}
}

void extended_hints()
{
	std::vector<std::uint32_t> stream;
	stream.reserve(65'409);
	NullMemory memory;
	const auto initial = state();
	auto interpreted = initial;
	interpreted.pc = base;
	for (unsigned immediate = 0; immediate <= 0xffffU; ++immediate) {
		if (is_primary_hint(immediate))
			continue;
		const auto instruction = hinte_instruction(immediate);
		stream.push_back(instruction);
		require(arm64::Dispatcher{}.execute(
			interpreted, memory, instruction).reason ==
			arm64::StopReason::continued);
	}
	require_unchanged(
		interpreted, initial, base + stream.size() * sizeof(std::uint32_t));

	stream.push_back(0xd400'0001U);
	const auto built = gadgets::Builder{}.build(base, stream);
	require(built.complete);
	require(built.program.instruction_count() == stream.size());
	auto gadget = initial;
	gadget.pc = base;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::system_call);
	require_unchanged(
		gadget, initial, base + stream.size() * sizeof(std::uint32_t));
}

} // namespace

int main()
{
	primary_hints();
	stateless_architectural_aliases();
	wait_with_timeout();
	extended_hints();
	return 0;
}
