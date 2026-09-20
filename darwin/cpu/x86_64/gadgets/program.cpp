// SPDX-License-Identifier: GPL-2.0-or-later
#include "program.hpp"
#include "executor.hpp"
#include "native_builder.hpp"
#include "native_entry.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace uml::cpu::x86_64::gadgets {

static_assert(std::is_trivially_copyable_v<Gadget>,
	"a translation copies its decoded instructions into its own storage");
static_assert(alignof(Gadget) <= alignof(std::max_align_t) &&
	alignof(Gadget) <= sizeof(std::uintptr_t),
	"the decoded instructions follow the native words without padding");

namespace {

[[nodiscard]] bool fatal_trace_enabled() noexcept
{
	const auto *value = std::getenv("UML_X86_FATAL_TRACE");
	return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void trace_fatal(const char *stage, const Program &program,
	const CpuState &cpu, std::size_t instruction_budget,
	std::size_t executed = 0) noexcept
{
	if (!fatal_trace_enabled())
		return;
	std::fprintf(stderr,
		"Darwin x86 gadget fatal stage=%s cpu-rip=%#llx program-rip=%#llx "
		"cpu-mode=%u program-mode=%u native-words=%zu instructions=%zu "
		"budget=%zu executed=%zu\n",
		stage,
		static_cast<unsigned long long>(cpu.rip),
		static_cast<unsigned long long>(program.address()),
		static_cast<unsigned>(cpu.execution_mode),
		static_cast<unsigned>(program.execution_mode()),
		program.native_stream().size(), program.instruction_count(),
		instruction_budget, executed);
	std::fflush(stderr);
}

/* The decoded instructions and the native words a translation assembles are
 * built into buffers the thread keeps, so the storage a build works in is
 * bought once per thread rather than once per block. Only the finished block
 * is allocated, at its own size. */
[[nodiscard]] std::vector<Gadget> &decoded_scratch() noexcept
{
	thread_local std::vector<Gadget> scratch;
	return scratch;
}

[[nodiscard]] std::vector<std::uintptr_t> &native_scratch() noexcept
{
	thread_local std::vector<std::uintptr_t> scratch;
	return scratch;
}

} // namespace

Program::Program(ExecutionMode mode, std::uint64_t address,
	std::size_t byte_count, std::span<const Gadget> gadgets,
	std::span<const std::byte> source, ProgramLinkResolver links)
	: mode_{mode}, address_{normalize_instruction_pointer(mode, address)},
	  byte_count_{byte_count}
{
	auto &words = native_scratch();
	words.clear();
	append_native_stream(words, gadgets,
		advance_instruction_pointer(mode_, address_, byte_count_), mode_,
		links);
	const auto word_bytes = words.size() * sizeof(std::uintptr_t);
	const auto gadget_bytes = gadgets.size() * sizeof(Gadget);
	storage_ = std::make_unique_for_overwrite<std::byte[]>(
		word_bytes + gadget_bytes + source.size());
	if (word_bytes != 0)
		std::memcpy(storage_.get(), words.data(), word_bytes);
	if (gadget_bytes != 0)
		std::memcpy(storage_.get() + word_bytes, gadgets.data(),
			gadget_bytes);
	if (!source.empty())
		std::memcpy(storage_.get() + word_bytes + gadget_bytes,
			source.data(), source.size());
	words_ = static_cast<std::uint32_t>(words.size());
	gadgets_ = static_cast<std::uint32_t>(gadgets.size());
	source_ = static_cast<std::uint32_t>(source.size());
}

BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, std::size_t maximum_instructions) noexcept

{
	return build(memory, address, ExecutionMode::long64,
		maximum_instructions);
}

BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions) noexcept
{
	return build(memory, address, mode, maximum_instructions, {});
}

BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions, ProgramLinkResolver links,
	std::span<std::byte> source) noexcept
{
	try {
		auto &stream = decoded_scratch();
		stream.clear();
		stream.reserve(maximum_instructions);
		auto current = normalize_instruction_pointer(mode, address);
		std::size_t bytes{};
		/* Only the instructions the block keeps are copied out, and only
		 * after they have decoded, so a failed decode's partial read and
		 * the bytes past the block's end never enter the source. */
		std::array<std::byte, maximum_instruction_bytes> consumed{};
		bool recorded = !source.empty();
		/* One window for the whole block: a run of instructions inside it is
		 * one fetch, and the bytes the decode uses are still the bytes the
		 * block is cached against. */
		FetchWindow window{};
		for (std::size_t index = 0; index < maximum_instructions; ++index) {
			const auto decoded = decode(memory, current, mode,
				recorded ? consumed.data() : nullptr, &window);
			if (decoded.status != DecodeStatus::decoded) {
				/* Preserve the side effects and retirement order of the valid
				 * prefix. Dispatch will decode the faulting instruction again
				 * after that prefix has executed.
				 */
				if (!stream.empty())
					break;
				if (decoded.status == DecodeStatus::fetch_fault)
					return {BuildStatus::fetch_fault, {},
						decoded.fault_address};
				return {BuildStatus::illegal_instruction, {}, current};
			}
			stream.push_back({current, decoded.instruction});
			if (recorded) {
				if (decoded.instruction.length <=
					source.size() - bytes) {
					std::memcpy(source.data() + bytes,
						consumed.data(),
						decoded.instruction.length);
				} else {
					recorded = false;
				}
			}
			current = advance_instruction_pointer(
				mode, current, decoded.instruction.length);
			bytes += decoded.instruction.length;
			if (decoded.instruction.terminal()) {
				/* A conditional branch's fall-through continues in this same
				 * block, so not taking the branch costs one dispatch instead
				 * of a link follow. Without a link resolver the branch has no
				 * taken record to enter and the block ends at it as before.
				 * The block still ends at an unconditional transfer or at the
				 * fetch window, so its size stays bounded. */
				if (decoded.instruction.operation !=
						Operation::jump_conditional ||
					links.resolve == nullptr)
					break;
			}
		}
		if (stream.empty())
			return {BuildStatus::illegal_instruction, {}, address};
		return {BuildStatus::built,
			Program{mode, address, bytes,
				std::span<const Gadget>{stream},
				recorded ? source.first(bytes) :
					std::span<const std::byte>{},
				links},
			0, recorded};
	} catch (const std::logic_error &) {
		return {BuildStatus::illegal_instruction, {}, address};
	} catch (...) {
		return {BuildStatus::allocation_failure, {}, 0};
	}
}

ExecuteResult execute(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt) noexcept

{
	try {
		Executor executor;
		executor.prepare(memory);
		return execute(program, cpu, memory, executor,
			instruction_budget, interrupt);
	} catch (...) {
		return {::uml::cpu::EngineEvent::fatal, 0, {}, false};
	}
}

ExecuteResult execute(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt) noexcept
{
	if (program.native_stream().empty() ||
		cpu.execution_mode != program.execution_mode() ||
		cpu.rip != program.address()) {
		trace_fatal("precondition", program, cpu, instruction_budget);
		return {::uml::cpu::EngineEvent::fatal, 0, {}, false};
	}
	if (interrupt.pending())
		return {::uml::cpu::EngineEvent::interrupt, 0, {}, false};
	if (instruction_budget == 0)
		return {::uml::cpu::EngineEvent::continued, 0, {}, true};

	std::vector<std::uintptr_t> limited_stream;
	std::span<const std::uintptr_t> native_stream = program.native_stream();
	const bool limited = instruction_budget < program.instruction_count();
	if (limited) {
		try {
			const auto gadgets = program.stream().first(instruction_budget);
			const auto final_rip = program.stream()[instruction_budget].address;
			limited_stream = build_native_stream(
				gadgets, final_rip, program.execution_mode());
			native_stream = limited_stream;
		} catch (...) {
			trace_fatal("limited-stream", program, cpu,
				instruction_budget);
			return {::uml::cpu::EngineEvent::fatal, 0, {}, false};
		}
	}

	NativeExecutionContext context{
		.memory = &memory,
		.tlb_entries = executor.tlb_entries(),
		.program_begin = program.address(),
		.program_end = program.address() + program.byte_count(),
		.unaligned_exclusives = executor.unaligned_exclusives(),
		.instruction_limit =
			std::min<std::size_t>(instruction_budget, 4096),
		.target_links = executor.target_links(program.execution_mode()),
		.execution_context = executor.execution_context(),
		.sequence_context = &executor,
		.code_change_reader = [](void *opaque) noexcept {
			return static_cast<Executor *>(opaque)->current_code_changes();
		},
		.code_changes = executor.prepared_code_changes(),
		.code_publications = 0,
		.code_pages = executor.code_page_registry(),
		.stack_entry = executor.tlb_entries(),
	};
	executor.enter_engine();
	const auto result = uml_x86_64_gadget_enter(
		native_stream.data(), &cpu, &context);
	executor.leave_engine();
	/* A block that published a code write and then left the stream never
	 * reached the link-following test that settles its own publication, so
	 * the sequence it leaves behind reads as somebody else's change and cools
	 * every link in the address space. Settle it here on the same terms: the
	 * global sequence advanced by exactly this run's own publications, each of
	 * which already cooled the blocks it actually overlapped.
	 */
	if (context.code_publications != 0 &&
		context.code_publications <=
			std::numeric_limits<std::uint64_t>::max() - context.code_changes) {
		const auto settled = context.code_changes + context.code_publications;
		if (executor.current_code_changes() == settled)
			(void)executor.accept_code_changes(settled);
	}
	const auto event = static_cast<::uml::cpu::EngineEvent>(result.event);
	if (event == ::uml::cpu::EngineEvent::continued &&
		context.indirect_miss != nullptr)
		executor.train_indirect(context.indirect_miss, cpu.rip, program.execution_mode());
	if (event == ::uml::cpu::EngineEvent::fatal)
		trace_fatal("native-entry", program, cpu, instruction_budget,
			result.executed);
	return {
		event,
		result.executed,
		context.fault,
		event == ::uml::cpu::EngineEvent::continued &&
			result.executed == instruction_budget,
	};
}

} // namespace uml::cpu::x86_64::gadgets
