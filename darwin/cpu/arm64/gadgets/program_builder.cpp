// SPDX-License-Identifier: GPL-2.0-or-later
#include "program_builder_decode.hpp"
#include "gadget_selection.hpp"
#include "pair_histogram.hpp"
#include "dependency_coverage.hpp"
#include "execution_histogram.hpp"
#include "scalar_forwarding.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
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

#if UML_GADGET_ELIDE_NOPS
/* Opt-in until measured: DARWIN_UML_GADGET_ELIDE_NOPS=1 leaves NOP entries
 * out of the stream, so a mechanism comparison shares one code layout. */
[[nodiscard]] bool elide_nops() noexcept
{
	static const bool enabled = [] {
		const char *setting = std::getenv("DARWIN_UML_GADGET_ELIDE_NOPS");
		return setting != nullptr && setting[0] == '1';
	}();
	return enabled;
}
#endif

[[nodiscard]] Program finish(
	const std::array<GuestRun, Program::maximum_runs> &runs,
	std::size_t run_count, std::size_t retired,
	ProgramWords words, bool accesses_fp_state)
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
void resolve_inline_suffixes(ProgramWords &words,
			     const InlineSuffixes &suffixes,
			     std::size_t compiled) noexcept
{
	for (std::uint32_t index = 0; index < suffixes.count; ++index) {
		const auto translated = suffixes.compiled[index];
		const auto suffix = translated >= compiled ?
			0 : compiled - translated;
		if (suffixes.shift[index] == 0)
			words[suffixes.word[index]] = suffix;
		else
			words[suffixes.word[index]] |=
				suffix << suffixes.shift[index];
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
	ProgramWords words{storage};
	InlineSuffixes suffixes{};
	std::size_t compiled = 0;
	/* The runs closed so far, and what the block has retired through them.
	 * A block is one run unless a call was translated inline into it. */
	std::array<GuestRun, Program::maximum_runs> runs{};
	std::size_t run_count = 0;
	std::size_t retired_before_run = 0;
	auto run_base = guest_address;
	std::array<std::uint32_t, 64> callee_window{};
	std::array<std::uint32_t, 64> return_window{};
	/* A transfer without a PC word names its instruction by a 16-bit
	 * position in the block, which an inlined call extends by at most the
	 * two windows above. */
	auto window = instructions.first(std::min(instructions.size(),
		(std::size_t{1} << 16) - callee_window.size() -
			return_window.size()));
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
#if UML_GADGET_DEPENDENCY_COVERAGE || UML_GADGET_SCALAR_FORWARDING
	std::uint16_t previous_scalar = scalar_no_register;
#endif
#if UML_GADGET_VECTOR_FORWARDING || UML_GADGET_DEPENDENCY_COVERAGE
	std::uint16_t previous_vector = scalar_no_register;
#endif
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
#if UML_GADGET_DEPENDENCY_COVERAGE || UML_GADGET_EXECUTION_HISTOGRAM
		const auto coverage_word = words.size();
		emit(words, diagnostics::uml_arm64_gadget_dependency_coverage);
		emit(words, std::uintptr_t{sizeof(std::uint64_t)});
		emit(words, std::uintptr_t{1});
#endif
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

#if UML_GADGET_DEPENDENCY_COVERAGE || UML_GADGET_SCALAR_FORWARDING || UML_GADGET_VECTOR_FORWARDING || UML_GADGET_EXECUTION_HISTOGRAM
		const auto compiled_before = compiled;
#endif
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
#if UML_GADGET_ELIDE_NOPS
		/* A guest word that translated to the NOP entry alone does nothing
		 * at run time: it cannot fault, retire out of order, or touch state.
		 * The block still retires it through its precharged length and its
		 * position stays addressable through the run list, so the entry and
		 * its dispatch are simply not emitted. Adjacent forwarding is not
		 * extended across it; the decode below sees nothing emitted. */
		if (elide_nops() && decoded == DecodeResult::decoded &&
		    words.size() == emitted_start + 1 &&
		    words[emitted_start] == reinterpret_cast<std::uintptr_t>(
			    uml_arm64_gadget_nop))
			words.pop_back();
#endif
#if UML_GADGET_VECTOR_FORWARDING || UML_GADGET_DEPENDENCY_COVERAGE
		const auto current_vector = decoded == DecodeResult::decoded &&
			compiled == compiled_before ?
			vector_access(std::span{words.data(), words.size()}.subspan(emitted_start)) : ScalarAccess{};
		const auto vector_mask = current_vector.source_mask(previous_vector);
		previous_vector = current_vector.destination;
#endif
#if UML_GADGET_DEPENDENCY_COVERAGE || UML_GADGET_SCALAR_FORWARDING
		// The selected assembly entry owns its access description. Do not
		// infer it again from opcode bits: aliases and fused groups differ.
		const auto current_scalar = decoded == DecodeResult::decoded &&
			compiled == compiled_before ?
			scalar_access(std::span{words.data(), words.size()}.subspan(emitted_start)) : ScalarAccess{};
		const auto source_mask = current_scalar.source_mask(previous_scalar);
#if UML_GADGET_DEPENDENCY_COVERAGE
		const auto bucket = current_vector.contract ? current_vector.coverage_bucket(
			vector_mask, diagnostics::vector_coverage_family) :
			current_scalar.coverage_bucket(source_mask);
		words[coverage_word + 1] = bucket * sizeof(std::uint64_t);
		words[coverage_word + 2] = std::max<std::size_t>(1,
			compiled - compiled_before + (decoded == DecodeResult::decoded));
#endif
#if UML_GADGET_SCALAR_FORWARDING
		if (source_mask != 0 && options.scalar_forwarding)
			words[emitted_start] = current_scalar.contract->entries[source_mask];
#endif
		previous_scalar = current_scalar.destination;
#endif
#if UML_GADGET_VECTOR_FORWARDING
		if (vector_mask != 0 && options.vector_forwarding)
			words[emitted_start] = current_vector.contract->entries[vector_mask];
#endif
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* The entry is final only after forwarding selection above. */
		if (words.size() > emitted_start) {
			words[coverage_word + 1] = diagnostics::execution_histogram_slot(
				previous_gadget,
				reinterpret_cast<const void *>(words[emitted_start]));
			words[coverage_word + 2] = std::max<std::size_t>(1,
				compiled - compiled_before +
				(decoded == DecodeResult::decoded));
		}
#endif
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
