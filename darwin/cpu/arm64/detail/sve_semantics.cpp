// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The semantic library's entry point and the helpers its sources share
 * (sve_semantics.hpp). Each source owns the handlers of its families of
 * forms and the lookup that routes a form to them.
 */
#include "sve_semantics_internal.hpp"

#include "cpu/arm64/detail/floating_exception.hpp"

namespace uml::cpu::arm64::detail {

namespace sve_semantics {

std::uint32_t predicate_test(
	const Predicate &mask, const Predicate &result, unsigned esize) noexcept
{
	bool first = true;
	bool n = false;
	bool z = true;
	bool c = true;
	for (unsigned element = 0; element < vector_bytes / esize; ++element) {
		if (!active(mask, element, esize))
			continue;
		const bool set = active(result, element, esize);
		if (first)
			n = set;
		first = false;
		z = z && !set;
		c = !set;
	}
	return (n ? 1U : 0U) << 31U | (z ? 1U : 0U) << 30U |
		(c ? 1U : 0U) << 29U;
}

unsigned predicate_count(unsigned pattern, unsigned esize) noexcept
{
	const unsigned elements = vector_bytes / esize;
	const auto fixed = [elements](unsigned count) {
		return elements >= count ? count : 0U;
	};
	switch (pattern & 31U) {
	case 0: /* POW2 */
		return std::bit_floor(elements);
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
	case 6:
	case 7: /* VL1-VL7 */
		return fixed(pattern & 31U);
	case 8: /* VL8 */
		return fixed(8);
	case 9: /* VL16 */
		return fixed(16);
	case 10: /* VL32 */
		return fixed(32);
	case 11: /* VL64 */
		return fixed(64);
	case 12: /* VL128 */
		return fixed(128);
	case 13: /* VL256 */
		return fixed(256);
	case 29: /* MUL4 */
		return elements - elements % 4U;
	case 30: /* MUL3 */
		return elements - elements % 3U;
	case 31: /* ALL */
		return elements;
	default:
		return 0;
	}
}

namespace {

[[nodiscard]] Handler handler_for(const SmeEncoding &row) noexcept
{
	if (row.memory != 0U)
		return memory_handler(row);
	const std::uint16_t form = row.form;
	for (const auto lookup :
		{predicate_handler, integer_handler, wide_handler,
			permute_handler, floating_handler, crypto_handler})
		if (const Handler handler = lookup(form); handler != nullptr)
			return handler;
	return nullptr;
}

[[nodiscard]] bool routed(const SmeEncoding &row) noexcept
{
	return (row.form & sme_form_sme_section) == 0U &&
		row.non_streaming_requirement != 0U;
}

/*
 * A floating-point form while an FPCR trap enable is set: AdvSIMD's
 * transactional path (the gadget and JIT executors). The form runs with
 * the cumulative flags clear; when its active elements raised an enabled
 * exception, the registers floating-point forms write (Z, V and P) and
 * FPSR return to their old values, and the trap reports one cause
 * (decide_floating_exception()). Whether an Underflow traps follows the
 * form's rounding: a flush to zero never traps, and with FPCR.UFE set a
 * tiny result raises it even when exact.
 */
[[nodiscard]] SveResult execute_trapping(
	Context &context, Handler handler) noexcept
{
	CpuState &cpu = context.cpu;
	const auto vectors = cpu.vectors;
	const auto predicates = cpu.sve_p;
	const std::uint32_t original_fpsr = cpu.fpsr;
	cpu.fpsr &= ~fpsr_exception_cumulative_mask;
	const SveResult result = handler(context);
	const auto decision = decide_floating_exception(cpu.fpcr,
		original_fpsr, cpu.fpsr,
		trappable_floating_exceptions(cpu.fpcr, cpu.fpsr,
			floating_rounding(context),
			[&] { return context.tiny_result; }));
	if (result.event == SveEvent::retired && decision.traps()) {
		cpu.vectors = vectors;
		cpu.sve_p = predicates;
		cpu.fpsr = decision.committed_fpsr;
		return {.event = SveEvent::floating_point_trap,
			.syndrome = decision.syndrome};
	}
	cpu.fpsr = (cpu.fpsr & ~fpsr_exception_cumulative_mask) |
		(decision.committed_fpsr & fpsr_exception_cumulative_mask);
	return result;
}

} // namespace

} // namespace sve_semantics

bool sve_semantics_covers(const SmeEncoding &row) noexcept
{
	return sve_semantics::routed(row) &&
		sve_semantics::handler_for(row) != nullptr;
}

SveResult execute_sve(CpuState &cpu, std::uint32_t word, const SmeEncoding &row,
	SveMemory &memory) noexcept
{
	using namespace sve_semantics;
	if ((word & row.mask) != row.value || !routed(row))
		return fatal();
	const Handler handler = handler_for(row);
	if (handler == nullptr)
		return fatal();
	cpu.access_fp_state();
	Context context{cpu, word, row, memory};
	const SveResult result =
		(cpu.fpcr & fpcr_exception_enable_mask) != 0U &&
			floating_handler(row.form) != nullptr ?
		execute_trapping(context, handler) :
		handler(context);
	if (result.event != SveEvent::fatal &&
		result.event != SveEvent::undefined)
		cpu.sve_live = 1;
	return result;
}

} // namespace uml::cpu::arm64::detail
