// SPDX-License-Identifier: GPL-2.0-or-later
#include "program_builder_decode.hpp"
#include "gadget_selection.hpp"
#include "pair_histogram.hpp"

#include <algorithm>
#include <span>
#include <utility>

/* Builder::build() below calls one of these five per guest instruction it
 * translates, and each of them calls on into the gadget selection tables.
 * Before the engine split that whole decode -> select -> emit chain lived in
 * one translation unit and the compiler inlined it freely; separating the
 * families into their own objects put hard call boundaries across the hottest
 * loop in translation and cost measurable score. Interprocedural optimization
 * recovered part of it but left single-core Clang, the most translation-heavy
 * workload, untouched, because ThinLTO imports across modules on a cost model
 * that declines functions this large. Composing them textually here is not a
 * shortcut around that decision: it is the only way to make the inliner see the
 * whole chain rather than leave it to a heuristic that has been measured to
 * decline. Each fragment still owns exactly one decode family. */
/* .ipp rather than .inc: every .inc in this tree is an assembly fragment of the
 * gadget bank, and editors do not treat .inc as C++. */
#include "program_builder_branch_system_impl.ipp"
#include "program_builder_data_processing_impl.ipp"
#include "program_builder_exception_impl.ipp"
#include "program_builder_floating_simd_impl.ipp"
#include "program_builder_load_store_impl.ipp"

namespace uml::cpu::arm64::gadgets {

using namespace selection;
using namespace builder_detail;

DecodeResult builder_detail::implementation_gap(DecodeContext &context)
{
	/* Keep a translated prefix executable. The terminal gadget reports the
	 * first unsupported allocation without charging it as a retired
	 * instruction; instruction_count deliberately remains the number of
	 * successfully translated words.
	 */
	emit(context.words, uml_arm64_gadget_illegal);
	emit(context.words, context.address);
	return DecodeResult::implementation_gap;
}

BuildResult Builder::build(
	std::uint64_t guest_address,
	std::span<const std::uint32_t> instructions,
	LinkResolver resolve_link, std::pmr::memory_resource *storage,
	IndirectLinkResolver resolve_indirect_link, BuildOptions options) const
{
	const ::uml::cpu::HostCapabilitiesScope capabilities_scope{
		options.host_capabilities};
	std::pmr::vector<std::uintptr_t> words{storage};
	words.reserve(std::min<std::size_t>(
		instructions.size() * 2 + 2, 34));
	std::size_t compiled = 0;
	bool program_accesses_fp_state = false;
	[[maybe_unused]] const void *previous_gadget = nullptr;
	/* Indexed rather than ranged, because a decoder that fuses a pair needs to
	 * see what follows the instruction it was handed. `compiled` is the loop
	 * position as well as the retired count, and a fusing decoder advances it
	 * past both instructions itself. */
	while (compiled < instructions.size()) {
		const auto instruction = instructions[compiled];
		program_accesses_fp_state = program_accesses_fp_state ||
			uml::cpu::arm64::detail::
				instruction_accesses_fp_state(instruction);
		const auto address = guest_address + compiled * sizeof(instruction);
		if ((instruction & 0xffff'0000U) == 0) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				true, address, instruction};
		}

		DecodeContext context{
			guest_address,
			address,
			instruction,
			instructions.subspan(compiled + 1),
			compiled,
			program_accesses_fp_state,
			words,
			resolve_link,
			resolve_indirect_link,
			options,
		};
		const auto emitted_start = words.size();
		DecodeResult decoded = DecodeResult::decoded;
		switch (production_decode_starts[instruction >> 24]) {
		case ProductionDecodeStart::branch_system:
			decoded = decode_branch_system(context);
			break;
		case ProductionDecodeStart::data_processing_immediate:
		case ProductionDecodeStart::data_processing_register:
			decoded = decode_data_processing(context);
			break;
		case ProductionDecodeStart::data_processing_pointer_auth_overlap:
			decoded = pointer_auth_lr_unavailable(instruction) ?
				decode_branch_system(context) :
				decode_data_processing(context);
			break;
		case ProductionDecodeStart::floating_simd:
			decoded = decode_floating_simd(context);
			break;
		case ProductionDecodeStart::load_store:
			decoded = decode_load_store(context);
			break;
		case ProductionDecodeStart::exception:
			decoded = decode_exception(context);
			break;
		}
		/* The first word a decode emits is its gadget pointer, which is
		 * what makes the adjacent-pair record cheap: no separate pass and
		 * no knowledge of operand shapes. Compiled out entirely unless the
		 * histogram build is selected. */
		if (words.size() > emitted_start) {
			const auto *emitted = reinterpret_cast<const void *>(
				words[emitted_start]);
			diagnostics::record_gadget_pair(previous_gadget, emitted);
			previous_gadget = emitted;
		}
		if (decoded != DecodeResult::decoded) {
			return {Program{guest_address, compiled, std::move(words),
				program_accesses_fp_state},
				decoded == DecodeResult::terminal,
				address, instruction};
		}
		++compiled;
	}
	const auto fallthrough = guest_address + compiled * sizeof(std::uint32_t);
	if (resolve_link) {
		emit(words, uml_arm64_gadget_branch_linked);
		emit(words, reinterpret_cast<std::uintptr_t>(
			resolve_link(fallthrough)));
	} else {
		emit(words, uml_arm64_gadget_interrupt);
		emit(words, fallthrough);
	}
	return {Program{guest_address, compiled, std::move(words),
		program_accesses_fp_state}, true,
		guest_address + compiled * sizeof(std::uint32_t), 0};
}

} // namespace uml::cpu::arm64::gadgets
