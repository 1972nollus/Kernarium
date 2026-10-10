// SPDX-License-Identifier: GPL-2.0-or-later
/* The gadget engine against the AArch32 instruction corpus
 * (tests/unit/arm64/aarch32_corpus/README.md owns the pack). Every case of
 * the page profile whose forms the engine's level implements is run through
 * the production AArch32 loop in the oracle's memory map, with the oracle's
 * exit stubs, and what the pack records is compared: the signal or system
 * call that ended it and the instruction and address it was raised at, every
 * general register, the APSR flags, Q and GE, TPIDRURW, the data region, and
 * the retired count (the pack counts the program's instructions; the engine
 * also retires the exit stub's load and, for a system call or a breakpoint,
 * that instruction).
 *
 * A case whose forms need a level above the engine's is skipped, as the
 * decoder-only runner skips them; the engine raises the illegal-instruction
 * exit for them, which the forms test checks. The `unaligned` and
 * `fpscr-reserved` suites are recorded only from hardware that does not exist
 * yet, so nothing here invents expectations for them: the engine follows the
 * design's rules (a multi-word access at a misaligned address is an
 * alignment fault, as the SCTLR.A = 0 text of the design says), and the
 * records of the `memory` suite stand for the aligned and unmapped cases.
 *
 *   uml_arm64_gadget_a32_corpus_tests PACK [--level base|simd|complete]
 *       [--record NAME] [--case TEXT] [--limit N] [--forms FILE]
 */
#include "aarch32_corpus_pack.hpp"
#include "arm64_gadget_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/gadgets/a32_support.hpp"
#include "scratch_backing.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace corpus = ::uml::testing::aarch32_corpus;
namespace decode = ::uml::cpu::arm64::aarch32;
using namespace ::uml::emulator::tests::a32;
using ::uml::testing::ScratchBacking;

constexpr std::uint32_t svc_word = 0xef00'0000U;
constexpr std::size_t budget = 20'000;

/* The oracle's memory map and exit stubs. */
class Machine final {
public:
	Machine()
		: generation_{3000}
		, code_{"a32corpus-code.XXXXXX", corpus::kCodeSize}
		, data_{"a32corpus-data.XXXXXX", corpus::kDataSize}
		, read_only_{"a32corpus-ro.XXXXXX", corpus::kReadOnlySize}
	{
		require(darwin_uml_static_vm_activate(generation_) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(std::uintptr_t{corpus::kCodeBase}),
			code_.descriptor(), 0, corpus::kCodeSize,
			PROT_READ | PROT_WRITE | PROT_EXEC, 1, 1) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(std::uintptr_t{corpus::kDataBase}),
			data_.descriptor(), 0, corpus::kDataSize,
			PROT_READ | PROT_WRITE, 0, 1) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(std::uintptr_t{corpus::kReadOnlyBase}),
			read_only_.descriptor(), 0, corpus::kReadOnlySize, PROT_READ,
			0, 0) == 0);
		const auto fill = corpus::read_only_fill();
		read_only_.put(0, std::as_bytes(std::span{fill}));
	}

	~Machine() { require(darwin_uml_static_vm_destroy(generation_) == 0); }

	Machine(const Machine &) = delete;
	Machine &operator=(const Machine &) = delete;

	[[nodiscard]] unsigned long generation() const noexcept
	{
		return generation_;
	}

	/* Code as the oracle prepares it: the program, an exit stub after it
	 * (a halfword NOP first when a Thumb program leaves the stub's word
	 * unaligned), the stubs at the two fixed exits, and this engine's exit
	 * code, a supervisor call, behind the two epilogues. */
	void install(const corpus::Case &test, std::uint32_t origin)
	{
		std::vector<std::uint8_t> image(corpus::kCodeSize, 0);
		const bool thumb = (test.flags & corpus::kFlagThumb) != 0;
		const auto put16 = [&](std::uint32_t address, std::uint16_t value) {
			std::memcpy(&image[address - corpus::kCodeBase], &value, 2);
		};
		const auto put32 = [&](std::uint32_t address, std::uint32_t value) {
			std::memcpy(&image[address - corpus::kCodeBase], &value, 4);
		};
		const auto arm_stub = [&](std::uint32_t address, std::uint32_t target) {
			put32(address, 0xe51ff004U); /* ldr pc, [pc, #-4] */
			put32(address + 4, target);
		};
		const auto thumb_stub = [&](std::uint32_t address, std::uint32_t target) {
			put16(address, 0xf8df); /* ldr.w pc, [pc, #0] */
			put16(address + 2, 0xf000);
			put32(address + 4, target);
		};
		for (std::size_t half = 0; half < test.halves.size(); ++half)
			put16(corpus::kCodeBase + origin + 2U * static_cast<std::uint32_t>(half),
			      test.halves[half]);
		/* The stub after the program exists only when it fits in the
		 * region, so a program at the end of the region falls off it. */
		const std::uint32_t end = corpus::kCodeBase + origin +
			2U * static_cast<std::uint32_t>(test.halves.size());
		const std::uint32_t limit = corpus::kCodeBase + corpus::kCodeSize;
		if (thumb) {
			const auto stub = (end + 3U) & ~3U;
			if (stub + 8 <= limit) {
				if (end % 4 != 0)
					put16(end, 0xbf00); /* nop */
				thumb_stub(stub, corpus::kThumbEpilogue);
			}
		} else if (end + 8 <= limit) {
			arm_stub(end, corpus::kArmEpilogue);
		}
		put32(corpus::kArmEpilogue, svc_word);
		put32(corpus::kThumbEpilogue, svc_word);
		arm_stub(corpus::kArmExit, corpus::kArmEpilogue);
		thumb_stub(corpus::kThumbExit, corpus::kThumbEpilogue);
		copy(corpus::kCodeBase, image);
	}

	/* Written through the guest mapping, so the host pages are already
	 * writable when an exclusive store reaches them (the first store to a
	 * shared page takes a fault that clears the host monitor, and the
	 * store-exclusive may then fail, as the architecture allows). */
	/* The static MMU copies one guest page at a time. */
	static void copy(std::uint32_t address, const std::vector<std::uint8_t> &bytes)
	{
		for (std::size_t offset = 0; offset < bytes.size(); offset += page)
			require(darwin_uml_static_vm_copy_from_buffer(address + offset,
				bytes.data() + offset,
				std::min<std::size_t>(page, bytes.size() - offset)) == 0);
	}

	void set_data(const std::vector<std::uint8_t> &bytes)
	{
		copy(corpus::kDataBase, bytes);
	}

	[[nodiscard]] std::vector<std::uint8_t> data() const
	{
		std::vector<std::uint8_t> bytes(corpus::kDataSize);
		data_.get(0, std::as_writable_bytes(std::span{bytes}));
		return bytes;
	}

	[[nodiscard]] ExecutionResult run(::uml::cpu::arm64::CpuState &cpu)
	{
		::uml::cpu::ExecutionRequest request{
			.cpu = cpu,
			.memory = memory_,
			.translated_memory = &memory_,
			.generation = generation_,
			.instruction_budget = budget,
		};
		return ::uml::cpu::arm64::detail::run_translated_aarch32_blocks(
			executor_, request);
	}

	/* The word at a code address, for the exits Linux turns into SIGTRAP. */
	[[nodiscard]] std::uint32_t code_word(std::uint32_t address)
	{
		std::uint32_t word = 0;
		(void)memory_.fetch(address,
			std::as_writable_bytes(std::span{&word, 1}));
		return word;
	}

private:
	unsigned long generation_;
	ScratchBacking code_;
	ScratchBacking data_;
	ScratchBacking read_only_;
	StaticGuestMemory memory_;
	::uml::cpu::arm64::gadgets::GadgetExecutor executor_;
};

/* The decoder's view of the program, to skip a case that needs a level the
 * engine does not implement. */
[[nodiscard]] bool needs_higher_level(const corpus::Case &test,
				      decode::Level level)
{
	const auto profile = decode::Profile::at(decode::Level::complete);
	const bool thumb = (test.flags & corpus::kFlagThumb) != 0;
	decode::ItState it;
	for (std::size_t half = 0; half < test.halves.size();) {
		decode::Instruction instruction;
		if (!thumb) {
			if (half + 1 >= test.halves.size())
				break;
			instruction = decode::decode_a32(
				std::uint32_t{test.halves[half + 1]} << 16 | test.halves[half],
				profile);
			half += 2;
		} else {
			const std::uint16_t first = test.halves[half];
			const auto length = decode::thumb_length(first);
			if (half + length / 2 > test.halves.size())
				break;
			const std::uint32_t word = length == 4 ?
				std::uint32_t{first} << 16 | test.halves[half + 1] :
				std::uint32_t{first} << 16;
			instruction = decode::decode_thumb(word, it, profile);
			it = instruction.next_it;
			half += length / 2;
		}
		if (instruction.legal() &&
		    static_cast<int>(instruction.info().level) > static_cast<int>(level))
			return true;
	}
	return false;
}

[[nodiscard]] bool linux_breakpoint(std::uint32_t word, bool thumb)
{
	/* arm64's compat layer, as arm's, raises SIGTRAP for these. */
	/* `word` is the guest's little-endian word at the PC: a T32
	 * instruction's first halfword is the low half. */
	return thumb ? (word & 0xffffU) == 0xde01U || word == 0xa000'f7f0U
		     : word == 0xe7f0'01f0U;
}

struct Outcome final {
	unsigned event{};
	bool exit_thumb{};
	std::uint32_t half{};
	std::uint32_t address{};
	std::size_t retired{};
	bool valid{true};
};

[[nodiscard]] Outcome classify(Machine &machine,
			       const ::uml::cpu::arm64::CpuState &cpu,
			       const ExecutionResult &result,
			       std::uint32_t entry, std::size_t halves)
{
	Outcome outcome;
	outcome.retired = result.retired_instructions;
	const auto pc = decode::key_pc(cpu.pc);
	const bool thumb = decode::key_is_thumb(cpu.pc);
	/* The pack numbers halfwords from the start of the program, and
	 * records 0 for a PC outside it: the fetch fault of a branch to an
	 * unfetchable target or of a program that falls off the region. */
	const auto half_of = [entry, halves](std::uint32_t address) {
		return address >= entry && (address - entry) / 2U < halves ?
			(address - entry) / 2U : 0U;
	};
	switch (result.reason) {
	case ExitReason::system_call:
		if (pc == corpus::kArmEpilogue + 4U) {
			outcome.event = 0;
		} else if (pc == corpus::kThumbEpilogue + 4U) {
			outcome.event = 0;
			outcome.exit_thumb = true;
		} else {
			outcome.event = corpus::kEventSvc;
			outcome.half = half_of(pc - (thumb ? 2U : 4U));
		}
		break;
	case ExitReason::memory_fault:
		outcome.event = result.fault.alignment ? corpus::kSigBus :
							 corpus::kSigSegv;
		outcome.half = half_of(pc);
		outcome.address = static_cast<std::uint32_t>(result.fault.address);
		break;
	case ExitReason::illegal_instruction:
		outcome.half = half_of(pc);
		outcome.address = pc;
		outcome.event = linux_breakpoint(machine.code_word(pc), thumb) ?
			corpus::kSigTrap : corpus::kSigIll;
		break;
	case ExitReason::breakpoint:
		outcome.event = corpus::kSigTrap;
		outcome.half = half_of(pc);
		outcome.address = pc;
		break;
	case ExitReason::pc_alignment:
		/* The fault is the target's; the branch that made it is the last
		 * instruction retired, which the engine does not name. */
		outcome.event = corpus::kSigBus;
		outcome.half = ~0U;
		outcome.address = static_cast<std::uint32_t>(result.fault.address);
		break;
	case ExitReason::instruction_budget:
		outcome.event = 14;
		break;
	default:
		outcome.valid = false;
		break;
	}
	return outcome;
}

void load_state(::uml::cpu::arm64::CpuState &cpu, const corpus::State &state,
		bool thumb, std::uint32_t entry)
{
	for (unsigned index = 0; index < corpus::kRegisters; ++index)
		cpu.registers[index] = state.r[index];
	cpu.nzcv = state.apsr & 0xf000'0000U;
	cpu.a32_q = static_cast<std::uint8_t>(state.apsr >> 27 & 1U);
	std::uint32_t mask = 0;
	for (unsigned lane = 0; lane < 4; ++lane)
		if (state.apsr >> (16 + lane) & 1U)
			mask |= 0xffU << (8 * lane);
	cpu.a32_ge_mask = mask;
	cpu.tls = state.tpidrurw;
	cpu.a32_tpidruro = state.tpidruro;
	cpu.a32_cp15ben = 1;
	cpu.a32 = 1;
	cpu.pc = thumb ? decode::t32_key(entry) : decode::a32_key(entry);
}

[[nodiscard]] std::uint32_t apsr_of(const ::uml::cpu::arm64::CpuState &cpu)
{
	std::uint32_t ge = 0;
	for (unsigned lane = 0; lane < 4; ++lane)
		if ((cpu.a32_ge_mask >> (8 * lane)) & 0xffU)
			ge |= 1U << (16 + lane);
	return (static_cast<std::uint32_t>(cpu.nzcv) & 0xf000'0000U) |
	       (std::uint32_t{cpu.a32_q} << 27) | ge;
}

struct Totals final {
	std::size_t run = 0, skipped = 0, failed = 0, printed = 0;
	/* The encodings under test of the cases that ran, for --forms. */
	std::set<std::string> forms;
};

struct Options final {
	decode::Level level = decode::Level::base;
	std::string record, text;
	std::size_t limit = 40;
	bool explain = false;
	/* Write the encodings whose cases ran, one per line, to this file. */
	std::string forms_path;
};

/* The program's words and what the decoder says of the instruction under
 * test at the engine's level and at the full profile. */
void explain(const corpus::Case &test, std::size_t target)
{
	std::fprintf(stderr, "    program %s:", test.flags & corpus::kFlagThumb ? "T32" : "A32");
	for (const auto half : test.halves)
		std::fprintf(stderr, " %04x", half);
	std::fprintf(stderr, "  target #%zu\n", target);
	const bool thumb = (test.flags & corpus::kFlagThumb) != 0;
	decode::ItState it;
	std::size_t index = 0;
	for (std::size_t half = 0; half < test.halves.size() && index <= target;) {
		std::uint32_t word;
		unsigned length;
		if (!thumb) {
			if (half + 1 >= test.halves.size())
				return;
			word = std::uint32_t{test.halves[half + 1]} << 16 | test.halves[half];
			length = 4;
		} else {
			length = decode::thumb_length(test.halves[half]);
			if (half + length / 2 > test.halves.size())
				return;
			word = length == 4 ? std::uint32_t{test.halves[half]} << 16 | test.halves[half + 1]
					   : std::uint32_t{test.halves[half]} << 16;
		}
		for (const auto level : {decode::Level::base, decode::Level::complete}) {
			const auto profile = decode::Profile::at(level);
			const auto instruction = thumb ? decode::decode_thumb(word, it, profile)
						       : decode::decode_a32(word, profile);
			if (index == target)
				std::fprintf(stderr, "    %s: %s status %d reason %d cond %u\n",
					     level == decode::Level::base ? "base" : "complete",
					     instruction.info().name,
					     static_cast<int>(instruction.status),
					     static_cast<int>(instruction.reason), instruction.cond);
			if (level == decode::Level::complete)
				it = instruction.next_it;
		}
		half += length / 2;
		++index;
	}
}

void check_case(Machine &machine, const corpus::Record &record,
		const corpus::Case &test, const Options &options, Totals &totals)
{
	if (needs_higher_level(test, options.level)) {
		++totals.skipped;
		return;
	}
	const bool thumb = (test.flags & corpus::kFlagThumb) != 0;
	const std::uint32_t origin = test.origin;
	auto initial_data = corpus::data_fill(test.fill);
	for (const auto &patch : test.data_patches)
		std::copy(patch.bytes.begin(), patch.bytes.end(),
			  initial_data.begin() + patch.offset);
	auto expected_data = initial_data;
	for (const auto &patch : test.memory_patches)
		std::copy(patch.bytes.begin(), patch.bytes.end(),
			  expected_data.begin() + patch.offset);

	machine.install(test, origin);
	machine.set_data(initial_data);
	::uml::cpu::arm64::CpuState cpu{};
	load_state(cpu, test.initial, thumb, corpus::kCodeBase + origin);
	const auto result = machine.run(cpu);
	const auto outcome = classify(machine, cpu, result,
				      corpus::kCodeBase + origin,
				      test.halves.size());
	++totals.run;
	totals.forms.insert(test.form);

	std::string diffs;
	const auto note = [&](const char *format, auto... values) {
		char text[200];
		std::snprintf(text, sizeof(text), format, values...);
		diffs += diffs.empty() ? "" : "; ";
		diffs += text;
	};
	if (!outcome.valid) {
		note("unexpected exit %d", static_cast<int>(result.reason));
	} else if (test.event == 14 || outcome.event == 14) {
		/* A loop neither side leaves: where it stops is not comparable. */
		if (test.event != outcome.event)
			note("event %u vs %u", test.event, outcome.event);
		if (diffs.empty())
			return;
	} else {
		if (test.event != outcome.event) {
			note("event %u vs %u", test.event, outcome.event);
		} else if (test.event == 0) {
			if (((test.flags & corpus::kFlagExitThumb) != 0) != outcome.exit_thumb)
				note("exit state %d vs %d",
				     (test.flags & corpus::kFlagExitThumb) != 0,
				     outcome.exit_thumb);
		} else {
			if (outcome.half != ~0U && test.fault_instruction != outcome.half)
				note("fault at halfword %u vs %u", test.fault_instruction,
				     outcome.half);
			if ((test.event == corpus::kSigSegv || test.event == corpus::kSigBus ||
			     test.event == corpus::kSigIll) &&
			    test.fault_address != outcome.address)
				note("fault address %#x vs %#x", test.fault_address,
				     outcome.address);
		}
		/* The pack counts the program's instructions: the engine adds the
		 * exit stub's load and the exit's system call, and a system call
		 * or a breakpoint itself. */
		std::size_t expected = test.retired;
		bool padded = false;
		if (test.event == 0) {
			expected += 2;
			/* A Thumb program that leaves the stub's word unaligned has
			 * a NOP before it, executed only when the program falls off
			 * its end. */
			padded = thumb && (corpus::kCodeBase + origin +
					   2U * test.halves.size()) % 4U != 0U;
		} else if (test.event == corpus::kEventSvc ||
			   result.reason == ExitReason::breakpoint) {
			expected += 1;
		}
		if (outcome.retired != expected && !(padded && outcome.retired == expected + 1))
			note("retired %zu vs %zu", test.retired, outcome.retired);
		for (unsigned index = 0; index < corpus::kRegisters; ++index) {
			const auto value = static_cast<std::uint32_t>(cpu.registers[index]);
			if (value != test.final.r[index])
				note("r%u %#x vs %#x", index, test.final.r[index], value);
		}
		const auto apsr = apsr_of(cpu);
		if (apsr != (test.final.apsr & 0xf80f'0000U))
			note("apsr %#x vs %#x", test.final.apsr & 0xf80f'0000U, apsr);
		if (static_cast<std::uint32_t>(cpu.tls) != test.final.tpidrurw)
			note("tpidrurw %#x vs %#x", test.final.tpidrurw,
			     static_cast<std::uint32_t>(cpu.tls));
		if (machine.data() != expected_data) {
			const auto actual = machine.data();
			std::size_t count = 0, first = 0;
			for (std::size_t index = 0; index < actual.size(); ++index)
				if (actual[index] != expected_data[index]) {
					if (count++ == 0)
						first = index;
				}
			note("data differs in %zu bytes, first at %#zx", count, first);
		}
	}
	if (diffs.empty())
		return;
	++totals.failed;
	if (totals.printed++ < options.limit) {
		std::fprintf(stderr, "%s: %s [%s]: %s\n", record.name.c_str(),
			     test.name.c_str(), test.form.c_str(), diffs.c_str());
		if (options.explain)
			explain(test, test.target);
	}
}

} // namespace

int main(int argc, char **argv)
{
	const ::uml::testing::RuntimeTestRoute runtime_route;
	const char *path = nullptr;
	Options options;
	options.level = static_cast<decode::Level>(
		::uml::cpu::arm64::gadgets::a32_support);
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--level" && index + 1 < argc) {
			const std::string_view name = argv[++index];
			require(name == "base" || name == "simd" || name == "complete",
				"unknown level");
			options.level = name == "base" ? decode::Level::base :
				name == "simd" ? decode::Level::simd :
						 decode::Level::complete;
		} else if (argument == "--record" && index + 1 < argc) {
			options.record = argv[++index];
		} else if (argument == "--case" && index + 1 < argc) {
			options.text = argv[++index];
		} else if (argument == "--forms" && index + 1 < argc) {
			options.forms_path = argv[++index];
		} else if (argument == "--explain") {
			options.explain = true;
		} else if (argument == "--limit" && index + 1 < argc) {
			options.limit = static_cast<std::size_t>(std::atoi(argv[++index]));
		} else {
			require(path == nullptr && !argument.starts_with("--"),
				"usage: PACK [--level L] [--record NAME] [--case TEXT]");
			path = argv[index];
		}
	}
	require(path != nullptr, "usage: PACK [--level L] [--record NAME]");
	const auto pack = corpus::read_pack(corpus::read_file(path));
	Machine machine;
	Totals totals;
	for (const auto &record : pack.records) {
		if ((record.profiles & corpus::kProfileBit) == 0 ||
		    (!options.record.empty() && record.name != options.record))
			continue;
		Totals before = totals;
		for (const auto &test : record.cases) {
			if (!options.text.empty() &&
			    test.name.find(options.text) == std::string::npos)
				continue;
			check_case(machine, record, test, options, totals);
		}
		std::printf("  %-24s run %5zu  skipped %5zu  failed %5zu\n",
			    record.name.c_str(), totals.run - before.run,
			    totals.skipped - before.skipped,
			    totals.failed - before.failed);
	}
	if (!options.forms_path.empty()) {
		std::FILE *out = std::fopen(options.forms_path.c_str(), "w");
		require(out != nullptr, "cannot write the form list");
		for (const auto &form : totals.forms)
			std::fprintf(out, "%s\n", form.c_str());
		std::fclose(out);
	}
	std::printf("arm64-gadget-a32-corpus: %zu cases run, %zu skipped above the "
		    "level, %zu failed\n",
		    totals.run, totals.skipped, totals.failed);
	return totals.failed == 0 ? 0 : 1;
}
