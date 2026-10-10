// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Checks the generated AArch32 instruction corpus. tests/unit/arm64/
 * aarch32_corpus/README.md owns the pack format and the oracles.
 *
 * No engine runs AArch32 yet, so a case is checked against what is known
 * without one: its program decodes, in order and in the IT state the earlier
 * instructions leave, to legal forms except at the instruction that raised an
 * event; the instruction under test is the form the case names; the event
 * is one the instruction's kind can raise; the retired count, the fault
 * position and address, the state changes and the memory runs are consistent
 * with that; and a case is stored once. Every case of a page profile runs; a
 * case whose forms need a level the run does not implement is skipped, so a
 * lane at the base or SIMD level reads the same pack.
 *
 *   uml_arm64_aarch32_corpus_tests PACK [--level base|simd|complete]
 *                                       [--record NAME]
 */
#include "aarch32_corpus_pack.hpp"
#include "cpu/arm64/aarch32/decode.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#ifndef DARWIN_UML_GUEST_PAGE_SIZE
#error "the guest page profile selects the corpus records"
#endif

namespace {

using ::uml::testing::require;
using namespace ::uml::cpu::arm64::aarch32;
using namespace ::uml::testing::aarch32_corpus;

/* Checking ---------------------------------------------------------------------- */

void require_in(const std::string &context, bool condition, const char *what)
{
	if (!condition) {
		std::fprintf(stderr, "%s: %s\n", context.c_str(), what);
		std::abort();
	}
}

struct Decoded final {
	Instruction instruction;
	unsigned half; /* halfword index of its first halfword */
};

std::vector<Decoded> decode_program(const std::string &context, const Case &test, const Profile &profile)
{
	std::vector<Decoded> result;
	const bool thumb = test.flags & kFlagThumb;
	ItState it;
	for (unsigned half = 0; half < test.halves.size();) {
		Instruction instruction;
		if (!thumb) {
			if (half + 1 >= test.halves.size()) {
				/* A program that switches state can end in a half of a word. */
				require_in(context, !(test.flags & kFlagStraight), "an A32 program is whole words");
				break;
			}
			instruction = decode_a32(std::uint32_t{test.halves[half + 1]} << 16 | test.halves[half], profile);
			result.push_back({instruction, half});
			half += 2;
			continue;
		}
		const std::uint16_t first = test.halves[half];
		const unsigned length = thumb_length(first);
		if (half + length / 2 > test.halves.size()) {
			/* The words of A32 code a program branches into end this way. */
			require_in(context, !(test.flags & kFlagStraight), "a Thumb program ends inside an instruction");
			break;
		}
		const std::uint32_t word =
			length == 4 ? std::uint32_t{first} << 16 | test.halves[half + 1] : std::uint32_t{first} << 16;
		instruction = decode_thumb(word, it, profile);
		it = instruction.next_it;
		result.push_back({instruction, half});
		half += length / 2;
	}
	return result;
}

const std::map<std::string, Form> &forms_by_name()
{
	static const std::map<std::string, Form> forms = [] {
		std::map<std::string, Form> result;
		for (std::size_t index = 0; index < kFormCount; ++index)
			result.emplace(kForms[index].name, static_cast<Form>(index));
		return result;
	}();
	return forms;
}

bool is_linux_breakpoint(const Instruction &instruction)
{
	/* The three encodings Linux turns into SIGTRAP (arm64's compat layer, as
	 * arm's): A32 0xe7f001f0, T16 0xde01 and T32 0xf7f0a000. */
	return instruction.word == 0xE7F001F0 || instruction.word == 0xDE010000 ||
		instruction.word == 0xF7F0A000;
}

struct Totals final {
	std::size_t records = 0, cases = 0, skipped = 0, events = 0;
	std::map<std::string, std::size_t> per_oracle;
};

void check_case(const Record &record, const Case &test, const Profile &profile, Level level, Totals &totals)
{
	const auto context = record.name + ": " + test.name;
	const auto fail = [&](const char *what) {
		std::fprintf(stderr, "%s: %s\n", context.c_str(), what);
		std::abort();
	};
	const auto program = decode_program(context, test, profile);
	const auto named = forms_by_name().find(test.form);
	if (named == forms_by_name().end())
		fail("names a form the decoder does not have");
	if (test.target >= program.size())
		fail("the instruction under test is not in the program");
	const Instruction &target = program[test.target].instruction;
	if (target.form != named->second)
		fail("the instruction under test decodes to a different form");
	for (const auto &decoded : program)
		if (static_cast<int>(decoded.instruction.info().level) > static_cast<int>(level) &&
			decoded.instruction.legal()) {
			++totals.skipped;
			return;
		}

	const bool thumb = test.flags & kFlagThumb;
	const bool straight = test.flags & kFlagStraight;
	const bool exit_thumb = test.flags & kFlagExitThumb;
	if (test.event == 0) {
		if (straight) {
			if (exit_thumb != thumb)
				fail("a straight program leaves in the instruction set it entered");
			if (test.retired != program.size())
				fail("a straight program retires all its instructions");
			for (const auto &decoded : program)
				if (!decoded.instruction.legal())
					fail("a program that finished holds an instruction the decoder rejects");
		}
	} else {
		++totals.events;
		if (test.event != kSigIll && test.event != kSigTrap && test.event != kSigBus &&
			test.event != kSigFpe && test.event != kSigSegv && test.event != kEventSvc && test.event != 14)
			fail("names an event that does not exist");
		const auto position = std::find_if(program.begin(), program.end(),
			[&](const Decoded &decoded) { return decoded.half == test.fault_instruction; });
		if (straight) {
			if (position == program.end())
				fail("the fault is not at an instruction");
			const auto ordinal = static_cast<unsigned>(position - program.begin());
			if (test.retired != ordinal)
				fail("a straight program retires the instructions before the fault");
			for (unsigned index = 0; index < ordinal; ++index)
				if (!program[index].instruction.legal())
					fail("an instruction ahead of the fault is rejected by the decoder");
			const Instruction &faulting = position->instruction;
			if (test.event == kSigIll) {
				/* UDF decodes and its execution is the undefined instruction. */
				if (faulting.legal() && !std::string_view(faulting.info().name).starts_with("UDF"))
					fail("SIGILL at an instruction the decoder accepts");
				if (test.fault_address != kCodeBase + 2U * test.fault_instruction)
					fail("SIGILL reports the instruction's address");
			} else if (test.event == kSigTrap) {
				if (!is_linux_breakpoint(faulting) &&
					!std::string_view(faulting.info().name).starts_with("BKPT"))
					fail("SIGTRAP at an instruction that is neither BKPT nor one of Linux's breakpoints");
			} else if (test.event == kEventSvc) {
				if (!std::string_view(faulting.info().name).starts_with("SVC"))
					fail("a system call event at an instruction that is not SVC");
			} else if (test.event == kSigSegv || test.event == kSigBus) {
				constexpr std::uint16_t memory = trait::loads | trait::stores | trait::exclusive | trait::prefetch;
				if (!faulting.legal() || !(faulting.info().traits & memory))
					fail("a memory fault at an instruction that does not access memory");
			}
		}
	}

	std::uint32_t end = 0;
	for (const auto &patch : test.memory_patches) {
		if (patch.offset < end || patch.offset + patch.length > kDataSize)
			fail("a changed data run is outside the data region");
		end = patch.offset + patch.length;
	}
	if ((test.final.apsr & ~0xF80F0000U) != 0 || (test.initial.apsr & ~0xF80F0000U) != 0)
		fail("an APSR holds bits other than N, Z, C, V, Q and GE");
	++totals.cases;
	++totals.per_oracle[record.oracle];
}

} // namespace

int main(int argc, char **argv)
{
	const char *path = nullptr;
	Level level = Level::complete;
	std::string only;
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--level" && index + 1 < argc) {
			const std::string_view name = argv[++index];
			level = name == "base" ? Level::base : name == "simd" ? Level::simd : Level::complete;
			require(name == "base" || name == "simd" || name == "complete", "unknown level");
		} else if (argument == "--record" && index + 1 < argc) {
			only = argv[++index];
		} else {
			require(path == nullptr && !argument.starts_with("--"), "usage: PACK [--level L] [--record NAME]");
			path = argv[index];
		}
	}
	require(path != nullptr, "usage: PACK [--level L] [--record NAME]");

	const Pack pack = read_pack(read_file(path));
	const Profile profile = Profile::at(Level::complete);
	Totals totals;
	std::set<std::string> names;
	for (const Record &record : pack.records) {
		require(names.insert(record.name).second, "a record name is used twice");
		if ((record.profiles & kProfileBit) == 0 || (!only.empty() && record.name != only))
			continue;
		++totals.records;
		std::set<std::tuple<std::string, std::string, std::uint16_t, std::vector<std::uint16_t>>> seen;
		for (const Case &test : record.cases) {
			require(seen.emplace(test.name, test.form, test.recipe, test.halves).second,
				"a case is stored twice");
			check_case(record, test, profile, level, totals);
		}
	}
	require(totals.records != 0, "no record applies to this page profile");
	std::printf("AArch32 corpus: %zu cases in %zu records (%zu raise an event, %zu skipped below the level)\n",
		totals.cases, totals.records, totals.events, totals.skipped);
	for (const auto &[oracle, count] : totals.per_oracle)
		std::printf("  %s: %zu\n", oracle.c_str(), count);
	return 0;
}
