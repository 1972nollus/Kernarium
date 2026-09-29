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
#include <type_traits>
#include <utility>
#include <vector>

namespace uml::cpu::x86_64::gadgets {

static_assert(std::is_trivially_copyable_v<Gadget>,
	"a replayed block copies its decoded instructions into its own storage");
static_assert(alignof(Gadget) <= alignof(std::max_align_t),
	"the retained instructions are allocated as bytes");

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

[[nodiscard]] std::vector<std::byte> &source_scratch() noexcept
{
	thread_local std::vector<std::byte> scratch;
	return scratch;
}

/* Where a replay's decode lands before the program keeps it. A build is
 * finished before any of this runs, so the replay has a buffer of its own
 * rather than sharing the build's. */
[[nodiscard]] std::vector<Gadget> &replay_scratch() noexcept
{
	thread_local std::vector<Gadget> scratch;
	return scratch;
}

/* Decode a program's instructions back out of the bytes its own build read.
 * Those bytes are the block, so this reproduces exactly the instructions the
 * block was built from -- deterministically, and without reading guest memory,
 * which has been free to change since and whose newer contents are no business
 * of a block already entered. The block's instructions end where its source
 * does; anything else means the two do not describe the same block.
 */
[[nodiscard]] bool decode_block(const Program &program,
	std::vector<Gadget> &gadgets) noexcept
{
	try {
		const auto source = program.source();
		const auto mode = program.execution_mode();
		gadgets.clear();
		gadgets.reserve(program.instruction_count());
		auto current = program.address();
		std::size_t offset = 0;
		for (std::size_t index = 0; index < program.instruction_count();
			++index) {
			if (offset >= source.size())
				break;
			const auto decoded = decode(
				source.subspan(offset), current, mode);
			if (decoded.status != DecodeStatus::decoded)
				break;
			gadgets.push_back({current, decoded.instruction});
			offset += decoded.instruction.length;
			current = advance_instruction_pointer(
				mode, current, decoded.instruction.length);
		}
		if (gadgets.size() == program.instruction_count() &&
			offset == source.size())
			return true;
		gadgets.clear();
		return false;
	} catch (...) {
		gadgets.clear();
		return false;
	}
}

} // namespace

std::span<const Gadget> Program::replay_instructions() const noexcept
{
	if (decoded_ != nullptr)
		return {reinterpret_cast<const Gadget *>(decoded_.get()),
			instructions_};
	auto &gadgets = replay_scratch();
	if (!decode_block(*this, gadgets))
		return {};
	const auto bytes = gadgets.size() * sizeof(Gadget);
	try {
		auto storage = std::make_unique_for_overwrite<std::byte[]>(bytes);
		std::memcpy(storage.get(), gadgets.data(), bytes);
		decoded_ = std::move(storage);
		return {reinterpret_cast<const Gadget *>(decoded_.get()),
			instructions_};
	} catch (...) {
		/* Without the storage this decode still stands for this one
		 * replay; the next one decodes again. */
		return {gadgets.data(), gadgets.size()};
	}
}

Program::Program(ExecutionMode mode, std::uint64_t address,
	std::size_t byte_count, std::span<const Gadget> gadgets,
	std::span<const std::byte> source, std::span<const std::uintptr_t> words)
	: address_{normalize_instruction_pointer(mode, address)},
	  byte_count_{byte_count}, mode_{mode}
{
	words_storage_ =
		std::make_unique_for_overwrite<std::uintptr_t[]>(words.size());
	source_storage_ =
		std::make_unique_for_overwrite<std::byte[]>(source.size());
	if (!words.empty())
		std::memcpy(words_storage_.get(), words.data(), words.size_bytes());
	if (!source.empty())
		std::memcpy(source_storage_.get(), source.data(), source.size());
	words_ = static_cast<std::uint32_t>(words.size());
	instructions_ = static_cast<std::uint32_t>(gadgets.size());
	source_ = static_cast<std::uint32_t>(source.size());
	terminal_ = !gadgets.empty() && gadgets.back().instruction.terminal();
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
	std::size_t maximum_instructions, ProgramLinkResolver links) noexcept
{
	try {
		auto &stream = decoded_scratch();
		stream.clear();
		stream.reserve(maximum_instructions);
		auto &source = source_scratch();
		source.clear();
		auto current = normalize_instruction_pointer(mode, address);
		std::size_t bytes{};
		/* Only the instructions the block keeps are copied out, and only
		 * after they have decoded, so a failed decode's partial read and
		 * the bytes past the block's end never enter the source. */
		std::array<std::byte, maximum_instruction_bytes> consumed{};
		/* One window for the whole block: a run of instructions inside it is
		 * one fetch, and the bytes the decode uses are still the bytes the
		 * block is cached against. */
		FetchWindow window{};
		for (std::size_t index = 0; index < maximum_instructions; ++index) {
			const auto decoded = decode(memory, current, mode,
				consumed.data(), &window);
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
			source.insert(source.end(), consumed.data(),
				consumed.data() + decoded.instruction.length);
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
		/* A gadget the bank has no form for ends the block before it, as a
		 * decode failure does: the prefix retires, and dispatch reports the
		 * refused instruction at its own address when it starts a block. */
		auto &words = native_scratch();
		for (;;) {
			words.clear();
			const auto refused = append_native_stream(words,
				std::span<const Gadget>{stream},
				advance_instruction_pointer(mode,
					normalize_instruction_pointer(mode, address), bytes),
				mode, links);
			if (!refused)
				break;
			if (*refused == 0)
				return {BuildStatus::illegal_instruction, {}, address};
			stream.resize(*refused);
			bytes = 0;
			for (const auto &gadget : stream)
				bytes += gadget.instruction.length;
			source.resize(bytes);
		}
		/* The saved bytes are the block: one instruction's encoding after
		 * another, ending where the block does. */
		return {BuildStatus::built,
			Program{mode, address, bytes,
				std::span<const Gadget>{stream},
				std::span<const std::byte>{source},
				std::span<const std::uintptr_t>{words}},
			0};
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
			const auto gadgets = program.replay_instructions();
			if (gadgets.size() != program.instruction_count()) {
				trace_fatal("limited-replay", program, cpu,
					instruction_budget);
				return {::uml::cpu::EngineEvent::fatal, 0, {},
					false};
			}
			limited_stream = build_native_stream(
				gadgets.first(instruction_budget),
				gadgets[instruction_budget].address,
				program.execution_mode());
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
	/* RF is cleared as the next instruction starts, after the
	 * instruction-breakpoint check it exists to suppress. It reaches the
	 * engine only from IRET, which always leaves it, and in the flags the
	 * kernel hands over, so that instruction always starts here. The exits
	 * above run no instruction and keep it, as an interrupt taken before the
	 * instruction does. */
	cpu.rflags &= ~rflags_resume;
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
