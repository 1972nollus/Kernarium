// SPDX-License-Identifier: GPL-2.0-or-later
#include "program_builder_decode.hpp"
#include "gadget_selection.hpp"
#include "pair_histogram.hpp"

#include <algorithm>
#include <array>
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

namespace {

[[nodiscard]] Program finish(
	const std::array<GuestRun, Program::maximum_runs> &runs,
	std::size_t run_count, std::size_t retired,
	std::pmr::vector<std::uintptr_t> words, bool accesses_fp_state)
{
	if (run_count <= 1)
		return Program{runs[0].address, retired, std::move(words),
			accesses_fp_state};
	return Program{std::span<const GuestRun>{runs.data(), run_count},
		retired, std::move(words), accesses_fp_state};
}



/* Fill in the reserved suffix operand of each inlined conditional branch, now
 * that the block's translated length is final. A taken branch subtracts this
 * from the count its block was precharged, so it is the instructions after the
 * branch itself. */
void resolve_inline_suffixes(std::pmr::vector<std::uintptr_t> &words,
			     const InlineSuffixes &suffixes,
			     std::size_t compiled) noexcept
{
	for (std::uint32_t index = 0; index < suffixes.count; ++index) {
		const auto translated = suffixes.compiled[index];
		words[suffixes.word[index]] = translated >= compiled ?
			0 : compiled - translated;
	}
}

} // namespace

BuildResult Builder::build(
	std::uint64_t guest_address,
	std::span<const std::uint32_t> instructions,
	LinkResolver resolve_link, std::pmr::memory_resource *storage,
	IndirectLinkResolver resolve_indirect_link, BuildOptions options,
	LinkRelocations *relocations, const InlineCalls *inline_calls,
	std::span<std::uint32_t> consumed_out, std::size_t *consumed_count) const
{
	const ::uml::cpu::HostCapabilitiesScope capabilities_scope{
		options.host_capabilities};
	std::pmr::vector<std::uintptr_t> words{storage};
	InlineSuffixes suffixes{};
	words.reserve(std::min<std::size_t>(
		instructions.size() * 2 + 2, 34));
	std::size_t compiled = 0;
	/* The runs closed so far, and what the block has retired through them.
	 * A block is one run unless a call was translated inline into it. */
	std::array<GuestRun, Program::maximum_runs> runs{};
	std::size_t run_count = 0;
	std::size_t retired_before_run = 0;
	auto run_base = guest_address;
	auto window = instructions;
	std::array<std::uint32_t, 64> callee_window{};
	std::array<std::uint32_t, 64> return_window{};
	std::uint64_t continue_at = 0;
	std::uint64_t continue_return = 0;
	std::uint64_t inlined_return = 0;
	/* Every guest word the build read, in run order, which is what the
	 * stale-program check compares against. */

	if (consumed_count != nullptr)
		*consumed_count = 0;
	/* A run retires `through` instructions, but a build that stopped read one
	 * more word than it retired, and the stale-program check compares every
	 * word the build read. */
	const auto close_run = [&](std::size_t through, std::size_t read) {
		if (run_count < runs.size())
			runs[run_count] = {run_base,
				static_cast<std::uint32_t>(through)};
		++run_count;
		if (consumed_count != nullptr)
			for (std::size_t index = 0; index < read &&
			     index < window.size() &&
			     *consumed_count < consumed_out.size(); ++index)
				consumed_out[(*consumed_count)++] = window[index];
		retired_before_run += through;
	};
	bool program_accesses_fp_state = false;
	[[maybe_unused]] const void *previous_gadget = nullptr;
	/* Indexed rather than ranged, because a decoder that fuses a pair needs to
	 * see what follows the instruction it was handed. `compiled` is the loop
	 * position as well as the retired count, and a fusing decoder advances it
	 * past both instructions itself. */
	while (compiled < window.size()) {
		const auto instruction = window[compiled];
		program_accesses_fp_state = program_accesses_fp_state ||
			uml::cpu::arm64::detail::
				instruction_accesses_fp_state(instruction);
		const auto address = run_base + compiled * sizeof(instruction);
		if ((instruction & 0xffff'0000U) == 0) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			close_run(compiled, compiled);
			resolve_inline_suffixes(words, suffixes, retired_before_run);
			return {finish(runs, run_count, retired_before_run,
				std::move(words), program_accesses_fp_state),
				true, address, instruction};
		}

		DecodeContext context{
			guest_address,
			address,
			instruction,
			window.subspan(compiled + 1),
			compiled,
			program_accesses_fp_state,
			words,
			resolve_link,
			resolve_indirect_link,
			options,
			relocations,
			&suffixes,
			retired_before_run,
			inline_calls != nullptr ? &continue_at : nullptr,
			inline_calls != nullptr ? &continue_return : nullptr,
			inlined_return,
			inline_calls != nullptr ? &inline_calls->may_inline : nullptr,
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
		if (decoded == DecodeResult::continue_elsewhere) {
			/* The run ends here and the next one starts wherever the
			 * decoder pointed. A window it cannot read, or a fourth
			 * run, simply leaves the block ending at what has been
			 * translated so far -- the stream already exits through
			 * the gadget the decoder emitted. */
			const auto entering = continue_return != 0;
			auto &buffer = entering ? callee_window : return_window;
			const auto count = inline_calls->fetch(continue_at,
				std::span{buffer});
			if (count == 0 || run_count + 2 > runs.size()) {
				/* The gadget already emitted assumes the next
				 * run follows it in the stream. Without one the
				 * block would simply run off the end, so it
				 * exits through a link to where it would have
				 * continued -- which is exactly the ordinary
				 * unlinked call or return. */
				emit(words, uml_arm64_gadget_branch_linked);
				if (relocations != nullptr)
					relocations->add(words.size(), false,
						continue_at);
				emit(words, reinterpret_cast<std::uintptr_t>(
					resolve_link(continue_at)));
				close_run(compiled, compiled);
				resolve_inline_suffixes(words, suffixes,
					retired_before_run);
				return {finish(runs, run_count, retired_before_run,
					std::move(words), program_accesses_fp_state),
					true, address, instruction};
			}
			close_run(compiled, compiled);
			run_base = continue_at;
			inlined_return = continue_return;
			window = std::span<const std::uint32_t>{buffer}.first(count);
			compiled = 0;
			continue;
		}
		if (decoded != DecodeResult::decoded) {
			close_run(compiled, compiled +
				(decoded == DecodeResult::implementation_gap ?
					1U : 0U));
			resolve_inline_suffixes(words, suffixes, retired_before_run);
			return {finish(runs, run_count, retired_before_run,
				std::move(words), program_accesses_fp_state),
				decoded == DecodeResult::terminal,
				address, instruction};
		}
		++compiled;
	}
	const auto fallthrough = run_base + compiled * sizeof(std::uint32_t);
	if (resolve_link) {
		emit(words, uml_arm64_gadget_branch_linked);
		if (relocations != nullptr)
			relocations->add(words.size(), false, fallthrough);
		emit(words, reinterpret_cast<std::uintptr_t>(
			resolve_link(fallthrough)));
	} else {
		emit(words, uml_arm64_gadget_interrupt);
		emit(words, fallthrough);
	}
	close_run(compiled, compiled);
	resolve_inline_suffixes(words, suffixes, retired_before_run);
	return {finish(runs, run_count, retired_before_run, std::move(words),
		program_accesses_fp_state), true,
		run_base + compiled * sizeof(std::uint32_t), 0};
}

} // namespace uml::cpu::arm64::gadgets
