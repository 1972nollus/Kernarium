// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The AArch32 instruction corpus on the semantic library: the corpus's model
 * lane. Every case of the pack runs through decode_a32() / decode_thumb()
 * and execute_a32() on the oracle's memory map, and its final registers, APSR,
 * FPSCR, D registers, TPIDRs, data bytes, event, fault address and retired
 * count are compared with what the oracle observed, as the engines' lanes
 * compare them. tests/unit/arm64/aarch32_corpus/README.md owns the pack and
 * its format.
 *
 * The model's guest pages are separate host allocations, so a library that
 * assumed two guest pages are adjacent in the host would fail here.
 *
 *   uml_arm64_aarch32_semantics_corpus_tests PACK [--record NAME]
 *       [--case NAME] [--form PREFIX] [--all-failures] [--trace] [--skipped]
 */
#include "aarch32_corpus_pack.hpp"
#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/semantics.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef DARWIN_UML_GUEST_PAGE_SIZE
#error "the guest page profile selects the corpus records"
#endif

namespace {

using ::uml::memory::MemoryAccess;
using ::uml::testing::require;
using namespace ::uml::cpu::arm64;
using namespace ::uml::cpu::arm64::aarch32;
using namespace ::uml::testing::aarch32_corpus;

constexpr std::uint32_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
/* Not an event of the pack: the library refused the word. */
constexpr unsigned kFatal = 0xfe;
/* The FPSCR halves the virtual CPU holds: AHP, DN, FZ, RMode and FZ16 in
 * fpcr; QC and the cumulative flags in fpsr; N, Z, C and V in
 * a32_fpscr_nzcv. */
constexpr std::uint32_t kFpscrControlMask = 0x07c8'0000U;
constexpr std::uint32_t kFpscrStatusMask = 0x0800'009fU;

/* --- The machine ---------------------------------------------------------------- */

class Machine final : public A32Memory {
public:
	Machine()
	{
		map(kCodeBase, kCodeSize, true, true, true);
		map(kDataBase, kDataSize, true, true, false);
		map(kReadOnlyBase, kReadOnlySize, true, false, false);
	}

	std::byte *translate(std::uint64_t address, MemoryAccess access) noexcept override
	{
		const auto found = pages_.find(static_cast<std::uint32_t>(address / page_size));
		if (found == pages_.end() || address >> 32 != 0)
			return nullptr;
		const Page &page = found->second;
		if ((access == MemoryAccess::read && !page.read) ||
			(access == MemoryAccess::write && !page.write) ||
			(access == MemoryAccess::execute && !page.execute))
			return nullptr;
		return page.bytes.get() + address % page_size;
	}

	void stored(std::byte *, std::size_t) noexcept override { ++stores_; }

	[[nodiscard]] std::size_t stores() const { return stores_; }

	/* Lays the case out; returns whether a nop precedes the exit stub that
	 * ends the program. */
	[[nodiscard]] bool load(const Case &test)
	{
		const bool thumb = test.flags & kFlagThumb;
		std::vector<std::uint8_t> code(kCodeSize, 0);
		const auto put16 = [&](std::uint32_t offset, std::uint16_t value) {
			std::memcpy(code.data() + offset, &value, 2);
		};
		const auto put32 = [&](std::uint32_t offset, std::uint32_t value) {
			std::memcpy(code.data() + offset, &value, 4);
		};
		for (std::size_t index = 0; index < test.halves.size(); ++index)
			put16(test.origin + static_cast<std::uint32_t>(2 * index), test.halves[index]);
		std::uint32_t end = test.origin + static_cast<std::uint32_t>(2 * test.halves.size());
		bool padded = false;
		const auto arm_stub = [&](std::uint32_t at) {
			put32(at, 0xe51ff004U); /* ldr pc, [pc, #-4] */
			put32(at + 4, kArmEpilogue);
		};
		const auto thumb_stub = [&](std::uint32_t at) {
			put16(at, 0xf8df); /* ldr.w pc, [pc, #0] */
			put16(at + 2, 0xf000);
			put32(at + 4, kThumbEpilogue);
		};
		/* A program at the end of the region has no room for a stub and falls
		 * off it. */
		if (thumb) {
			if (end % 4 != 0 && end + 2 + 8 <= kCodeSize) {
				padded = true;
				put16(end, 0xbf00); /* nop, so the stub is word aligned */
				end += 2;
			}
			if (end + 8 <= kCodeSize)
				thumb_stub(end);
		} else if (end + 8 <= kCodeSize) {
			arm_stub(end);
		}
		arm_stub(kArmExit - kCodeBase);
		thumb_stub(kThumbExit - kCodeBase);
		put32(kArmEpilogue - kCodeBase, 0);
		put32(kThumbEpilogue - kCodeBase, 0);
		fill(kCodeBase, code.data(), kCodeSize);

		auto data = data_fill(test.fill);
		for (const auto &patch : test.data_patches)
			std::copy(patch.bytes.begin(), patch.bytes.end(), data.begin() + patch.offset);
		fill(kDataBase, data.data(), kDataSize);
		expected_data_ = data;
		const auto read_only = read_only_fill();
		fill(kReadOnlyBase, read_only.data(), kReadOnlySize);
		return padded;
	}

	[[nodiscard]] std::vector<std::uint8_t> expected_memory(const Case &test) const
	{
		auto memory = expected_data_;
		for (const auto &patch : test.memory_patches)
			std::copy(patch.bytes.begin(), patch.bytes.end(), memory.begin() + patch.offset);
		return memory;
	}

	[[nodiscard]] std::vector<std::uint8_t> read_data() const
	{
		std::vector<std::uint8_t> bytes(kDataSize);
		for (std::uint32_t offset = 0; offset < kDataSize; offset += page_size)
			std::memcpy(bytes.data() + offset,
				pages_.at((kDataBase + offset) / page_size).bytes.get(),
				std::min(page_size, kDataSize - offset));
		return bytes;
	}

	/* The bytes a fetch reads, or false when the page cannot be executed. */
	[[nodiscard]] bool fetch(std::uint32_t address, void *out, std::size_t size)
	{
		const std::byte *host = translate(address, MemoryAccess::execute);
		if (host == nullptr)
			return false;
		std::memcpy(out, host, size);
		return true;
	}

private:
	struct Free final {
		void operator()(std::byte *pointer) const noexcept { std::free(pointer); }
	};
	struct Page final {
		std::unique_ptr<std::byte, Free> bytes;
		bool read{}, write{}, execute{};
	};

	void map(std::uint32_t base, std::uint32_t size, bool read, bool write, bool execute)
	{
		for (std::uint32_t offset = 0; offset < size; offset += page_size) {
			Page page;
			void *memory = std::aligned_alloc(page_size, page_size);
			require(memory != nullptr, "out of memory");
			std::memset(memory, 0, page_size);
			page.bytes.reset(static_cast<std::byte *>(memory));
			page.read = read;
			page.write = write;
			page.execute = execute;
			pages_.emplace((base + offset) / page_size, std::move(page));
		}
	}

	void fill(std::uint32_t base, const std::uint8_t *source, std::uint32_t size)
	{
		for (std::uint32_t offset = 0; offset < size; offset += page_size)
			std::memcpy(pages_.at((base + offset) / page_size).bytes.get(), source + offset,
				std::min(page_size, size - offset));
	}

	std::map<std::uint32_t, Page> pages_;
	std::vector<std::uint8_t> expected_data_;
	std::size_t stores_{};
};

/* --- Running a case ------------------------------------------------------------- */

CpuState cpu_from(const State &state, bool thumb, std::uint32_t entry)
{
	CpuState cpu{};
	cpu.a32 = 1;
	cpu.pc = thumb ? t32_key(entry) : a32_key(entry);
	for (unsigned index = 0; index < kRegisters; ++index)
		cpu.registers[index] = state.r[index];
	cpu.nzcv = state.apsr & 0xf000'0000U;
	cpu.a32_q = static_cast<std::uint8_t>(state.apsr >> 27 & 1U);
	for (unsigned lane = 0; lane < 4; ++lane)
		if (state.apsr >> (16 + lane) & 1U)
			cpu.a32_ge_mask |= 0xffU << (8 * lane);
	cpu.a32_cp15ben = 1;
	cpu.a32_tpidruro = state.tpidruro;
	cpu.tls = state.tpidrurw;
	for (unsigned d = 0; d < 16 * 2; ++d)
		std::memcpy(cpu.vectors[d / 2].bytes.data() + 8U * (d & 1U), &state.d[d], 8);
	cpu.fpcr = state.fpscr & kFpscrControlMask;
	cpu.fpsr = state.fpscr & kFpscrStatusMask;
	cpu.a32_fpscr_nzcv = state.fpscr & 0xf000'0000U;
	return cpu;
}

std::uint32_t fpscr_of(const CpuState &cpu)
{
	return (cpu.fpcr & kFpscrControlMask) | (cpu.fpsr & kFpscrStatusMask) |
		(cpu.a32_fpscr_nzcv & 0xf000'0000U);
}

std::uint32_t apsr_of(const CpuState &cpu)
{
	std::uint32_t apsr = static_cast<std::uint32_t>(cpu.nzcv) & 0xf000'0000U;
	if (cpu.a32_q != 0U)
		apsr |= 1U << 27;
	for (unsigned lane = 0; lane < 4; ++lane)
		if ((cpu.a32_ge_mask >> (8 * lane) & 0xffU) != 0U)
			apsr |= 1U << (16 + lane);
	return apsr;
}

struct Outcome final {
	unsigned event{};
	std::size_t retired{};
	/* The address a SIGILL or SIGTRAP is at, or a fault's address. */
	std::uint32_t address{};
	bool finished_in_thumb{};
	bool budget_exhausted{};
};

/* The word at the PC, as the decoder holds it: A32 bits, hw1 << 16 | hw2 or
 * hw1 << 16 for a T16 instruction. */
std::uint32_t instruction_word(Machine &machine, std::uint32_t pc, bool thumb)
{
	if (!thumb) {
		std::uint32_t word = 0;
		(void)machine.fetch(pc, &word, 4);
		return word;
	}
	std::uint16_t first = 0, second = 0;
	(void)machine.fetch(pc, &first, 2);
	if (thumb_length(first) == 4)
		(void)machine.fetch(pc + 2, &second, 2);
	return thumb_length(first) == 4 ? std::uint32_t{first} << 16 | second : std::uint32_t{first} << 16;
}

bool g_trace = false;

/* Runs the program the machine holds from `cpu` until it reaches an
 * epilogue or raises an event: the loop an engine's run_aarch32 is. */
Outcome run(Machine &machine, CpuState &cpu, const Profile &profile)
{
	Outcome outcome;
	for (unsigned budget = 0; budget < 4096; ++budget) {
		const std::uint64_t key = cpu.pc;
		const std::uint32_t pc = key_pc(key);
		const bool thumb = key_is_thumb(key);
		if (!thumb && (pc == kArmEpilogue || pc == kThumbEpilogue)) {
			outcome.finished_in_thumb = pc == kThumbEpilogue;
			return outcome;
		}
		if (g_trace)
			std::fprintf(stderr, "  %08x %08x it=%02x\n", pc, instruction_word(machine, pc, thumb),
				cpu.a32_itstate);
		const StepResult step = step_a32(cpu, machine, profile);
		switch (step.event) {
		case StepEvent::retired:
			++outcome.retired;
			continue;
		case StepEvent::supervisor_call:
			outcome.event = kEventSvc;
			return outcome;
		case StepEvent::breakpoint:
			outcome.event = kSigTrap;
			outcome.address = pc;
			return outcome;
		case StepEvent::undefined: {
			/* The three undefined encodings Linux turns into SIGTRAP (arm64's
			 * compat layer, as arm's): A32 0xe7f001f0, T16 0xde01 and T32
			 * 0xf7f0a000. */
			const std::uint32_t word = instruction_word(machine, pc, thumb);
			outcome.event = word == 0xE7F001F0 || word == 0xDE010000 || word == 0xF7F0A000 ? kSigTrap : kSigIll;
			outcome.address = pc;
			return outcome;
		}
		case StepEvent::memory_fault:
			outcome.event = step.alignment ? kSigBus : kSigSegv;
			outcome.address = step.fault_address;
			return outcome;
		case StepEvent::pc_alignment:
			outcome.event = kSigBus;
			outcome.address = step.fault_address;
			return outcome;
		case StepEvent::fatal:
			outcome.event = kFatal;
			outcome.address = pc;
			return outcome;
		}
	}
	outcome.budget_exhausted = true;
	return outcome;
}

/* Returns the reason the library disagrees with the oracle, or nothing. */
std::string check(Machine &machine, const Case &test, const Profile &profile)
{
	const bool thumb = test.flags & kFlagThumb;
	const bool padded = machine.load(test);
	auto cpu = cpu_from(test.initial, thumb, kCodeBase + test.origin);
	Outcome outcome = run(machine, cpu, profile);

	char text[512];
	if (outcome.budget_exhausted)
		return "the program did not finish";
	if (outcome.event == 0) {
		if (outcome.retired < 1)
			return "the stub exit retired too little";
		--outcome.retired; /* the stub's load of the PC */
		if (padded && (test.flags & kFlagStraight) != 0)
			--outcome.retired; /* the nop that aligns a Thumb stub the program fell into */
		if (((test.flags & kFlagExitThumb) != 0) != outcome.finished_in_thumb)
			return "finished in the wrong instruction set";
	}
	if (outcome.event == kFatal) {
		std::snprintf(text, sizeof text, "the library reports fatal at %08x", outcome.address);
		return text;
	}
	if (outcome.event != test.event) {
		std::snprintf(text, sizeof text, "event %u, expected %u (retired %zu, address %08x)",
			outcome.event, test.event, outcome.retired, outcome.address);
		return text;
	}
	if (outcome.retired != test.retired) {
		std::snprintf(text, sizeof text, "retired %zu, expected %u", outcome.retired,
			test.retired);
		return text;
	}
	if (outcome.event != 0 && outcome.event != kEventSvc && outcome.address != test.fault_address) {
		std::snprintf(text, sizeof text, "fault address %08x, expected %08x", outcome.address,
			test.fault_address);
		return text;
	}
	for (unsigned index = 0; index < kRegisters; ++index) {
		if (static_cast<std::uint32_t>(cpu.registers[index]) != test.final.r[index] ||
			(cpu.registers[index] >> 32) != 0) {
			std::snprintf(text, sizeof text, "r%u = %016llx, expected %08x", index,
				static_cast<unsigned long long>(cpu.registers[index]), test.final.r[index]);
			return text;
		}
	}
	if (apsr_of(cpu) != test.final.apsr) {
		std::snprintf(text, sizeof text, "apsr %08x, expected %08x", apsr_of(cpu), test.final.apsr);
		return text;
	}
	if (cpu.a32_tpidruro != test.final.tpidruro || cpu.tls != test.final.tpidrurw)
		return "a thread register changed";
	for (unsigned d = 0; d < kVectors; ++d) {
		std::uint64_t value;
		std::memcpy(&value, cpu.vectors[d / 2].bytes.data() + 8U * (d & 1U), 8);
		if (value != test.final.d[d]) {
			std::snprintf(text, sizeof text, "d%u = %016llx, expected %016llx", d,
				static_cast<unsigned long long>(value),
				static_cast<unsigned long long>(test.final.d[d]));
			return text;
		}
	}
	if (fpscr_of(cpu) != test.final.fpscr) {
		std::snprintf(text, sizeof text, "fpscr %08x, expected %08x", fpscr_of(cpu), test.final.fpscr);
		return text;
	}
	const auto expected = machine.expected_memory(test);
	const auto actual = machine.read_data();
	if (expected != actual) {
		std::size_t at = 0;
		while (expected[at] == actual[at])
			++at;
		std::snprintf(text, sizeof text, "data differs at +%zx: %02x, expected %02x", at,
			static_cast<unsigned>(actual[at]), static_cast<unsigned>(expected[at]));
		return text;
	}
	return {};
}

/* Whether the library covers every legal instruction of the program. The
 * library implements the whole profile, so a case that is not within
 * coverage is a failure of the lane, not a case to leave out. */
bool within_coverage(const Case &test)
{
	const bool thumb = test.flags & kFlagThumb;
	const Profile complete = Profile::at(Level::complete);
	ItState it;
	for (unsigned half = 0; half < test.halves.size();) {
		Instruction instruction;
		if (!thumb) {
			if (half + 1 >= test.halves.size())
				break;
			const std::uint32_t word = std::uint32_t{test.halves[half + 1]} << 16 | test.halves[half];
			instruction = decode_a32(word, complete);
			half += 2;
		} else {
			const std::uint16_t first = test.halves[half];
			const unsigned length = thumb_length(first);
			if (half + length / 2 > test.halves.size())
				break;
			const std::uint32_t word = length == 4
				? std::uint32_t{first} << 16 | test.halves[half + 1] : std::uint32_t{first} << 16;
			instruction = decode_thumb(word, it, complete);
			it = instruction.next_it;
			half += length / 2;
		}
		if (instruction.legal() && !a32_semantics_covers(instruction.form))
			return false;
	}
	return true;
}

struct Totals final {
	std::size_t run = 0, skipped = 0, failed = 0;
	std::map<std::string, std::size_t> failed_forms;
	std::map<std::string, std::size_t> skipped_forms;
};

} // namespace

int main(int argc, char **argv)
{
	const char *path = nullptr;
	std::string only_record, only_case, only_form;
	bool all_failures = false;
	bool trace = false;
	bool list_skipped = false;
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--record" && index + 1 < argc)
			only_record = argv[++index];
		else if (argument == "--case" && index + 1 < argc)
			only_case = argv[++index];
		else if (argument == "--form" && index + 1 < argc)
			only_form = argv[++index];
		else if (argument == "--all-failures")
			all_failures = true;
		else if (argument == "--trace")
			trace = true;
		else if (argument == "--skipped")
			list_skipped = true;
		else
			path = argv[index];
	}
	require(path != nullptr, "usage: PACK [--record NAME] [--case NAME] [--form PREFIX]");
	g_trace = trace;
	const Pack pack = read_pack(read_file(path));
	/* The whole profile: the library implements all of it. */
	const Profile profile = Profile::at(Level::complete);

	Machine machine;
	Totals totals;
	for (const Record &record : pack.records) {
		if ((record.profiles & kProfileBit) == 0 || (!only_record.empty() && record.name != only_record))
			continue;
		for (const Case &test : record.cases) {
			if (!only_case.empty() && test.name != only_case)
				continue;
			if (!only_form.empty() && !test.form.starts_with(only_form))
				continue;
			if (!within_coverage(test)) {
				++totals.skipped;
				++totals.skipped_forms[test.form];
				continue;
			}
			++totals.run;
			if (trace) {
				std::fprintf(stderr, "case %s:", test.name.c_str());
				for (const auto half : test.halves)
					std::fprintf(stderr, " %04x", half);
				std::fprintf(stderr, "\n  fpscr %08x apsr %08x\n", test.initial.fpscr, test.initial.apsr);
				for (unsigned d = 0; d < 32; ++d)
					std::fprintf(stderr, "  d%u=%016llx%s", d,
						static_cast<unsigned long long>(test.initial.d[d]), d % 4 == 3 ? "\n" : "");
				for (unsigned r = 0; r < 15; ++r)
					std::fprintf(stderr, "  r%u=%08x%s", r, test.initial.r[r], r % 5 == 4 ? "\n" : "");
			}
			const std::string problem = check(machine, test, profile);
			if (problem.empty())
				continue;
			++totals.failed;
			++totals.failed_forms[test.form];
			if (totals.failed_forms[test.form] == 1 || all_failures || !only_case.empty()) {
				std::fprintf(stderr, "FAIL %s: %s: %s\n  program:", record.name.c_str(), test.name.c_str(),
					problem.c_str());
				for (const auto half : test.halves)
					std::fprintf(stderr, " %04x", half);
				std::fprintf(stderr, "%s\n", test.flags & kFlagThumb ? " (thumb)" : " (arm)");
			}
		}
	}
	std::printf("AArch32 corpus on the semantic library: %zu cases run, %zu skipped, "
		    "%zu failed\n", totals.run, totals.skipped, totals.failed);
	for (const auto &[form, count] : totals.failed_forms)
		std::printf("  %s: %zu\n", form.c_str(), count);
	if (list_skipped)
		for (const auto &[form, count] : totals.skipped_forms)
			std::printf("  skipped %s: %zu\n", form.c_str(), count);
	/* A case the library does not cover is a gap in the library. */
	if (totals.skipped != 0)
		std::fprintf(stderr, "%zu cases hold a form the library does not cover (see --skipped)\n",
			totals.skipped);
	return totals.failed == 0 && totals.skipped == 0 ? 0 : 1;
}
