// SPDX-License-Identifier: GPL-2.0-or-later
/* The gadget engine translates exactly what the shared decoder defines at its
 * level: a word the decoder accepts never ends in the illegal-instruction
 * exit, and one it rejects always does, at the word. Every stored A32, T16
 * and T32 sample word of the decoder's test data (the directory is the first
 * argument), and the whole 16-bit Thumb space, is placed in a guest, run, and
 * held to the decoder's answer for the engine's profile. A word accepted at
 * a level the engine does not yet implement therefore fails here, which is
 * what keeps the level honest. No form reaches a semantic library: the
 * gadget engine's AArch32 part has no library exit at this level, and the
 * loop it runs under (run_translated_aarch32_blocks) has no event for one.
 *
 * Words with a condition are run as unconditional ones: the decoder's answer
 * does not depend on the condition, and a failing condition would skip the
 * word and hide it. */
#include "arm64_gadget_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/gadgets/a32_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ::uml::emulator::tests::a32;
namespace decode = ::uml::cpu::arm64::aarch32;

constexpr std::uint16_t nop = 0xbf00;
constexpr std::size_t budget = 64;

struct Sample final {
	char isa; /* 'A', 'T' (T16) or 'W' (T32) */
	std::uint32_t word;
};

[[nodiscard]] std::vector<Sample> read_samples(const std::string &directory)
{
	std::ifstream input(directory + "/samples.tsv");
	require(input.is_open(), "the decoder test data is missing");
	std::vector<Sample> samples;
	std::string line;
	std::getline(input, line);
	while (std::getline(input, line)) {
		std::stringstream stream(line);
		std::string isa, word;
		std::getline(stream, isa, '\t');
		std::getline(stream, word, '\t');
		const auto value = static_cast<std::uint32_t>(
			std::strtoul(word.c_str(), nullptr, 16));
		samples.push_back({isa == "A32" ? 'A' : isa == "T16" ? 'T' : 'W',
			value});
	}
	return samples;
}

class Runner final {
public:
	Runner()
		: guest_{1100}
		, profile_{decode::Profile::at(static_cast<decode::Level>(
			  ::uml::cpu::arm64::gadgets::a32_support))}
	{
		/* The code around a word is benign in both instruction sets: as
		 * A32 a conditional supervisor call that fails its condition,
		 * as T32 two NOPs, so a branch that lands on it, or falls
		 * through to it, ends in the instruction budget. */
		guest_.fill_code(0xbf00'bf00U);
	}

	/* Runs one A32 word. */
	void a32(std::uint32_t word)
	{
		if ((word >> 28) < 14U)
			word = 0xe000'0000U | (word & 0x0fff'ffffU);
		const auto instruction = decode::decode_a32(word, profile_);
		guest_.a32(0, {word});
		guest_.code_changed();
		finish(instruction, arm64_state(false), word);
	}

	/* Runs one Thumb instruction (a halfword, or two for a 32-bit one). */
	void thumb(std::uint32_t word)
	{
		const auto first = static_cast<std::uint16_t>(word >> 16);
		const bool wide = first >= 0xe800;
		const auto hw1 = wide ? first : static_cast<std::uint16_t>(word);
		const auto instruction = decode::decode_thumb(
			wide ? word : std::uint32_t{hw1} << 16, decode::ItState{},
			profile_);
		if (wide)
			guest_.t32(0, {hw1, static_cast<std::uint16_t>(word)});
		else
			guest_.t32(0, {hw1, nop});
		guest_.code_changed();
		finish(instruction, arm64_state(true), word);
	}

	[[nodiscard]] std::size_t checked() const noexcept { return checked_; }
	[[nodiscard]] std::size_t accepted() const noexcept { return accepted_; }

private:
	/* Registers that make loads and stores land in the data, a stack, and
	 * a link register that returns into the benign code. */
	[[nodiscard]] static ::uml::cpu::arm64::CpuState arm64_state(bool thumb)
	{
		auto cpu = thumb ? t32_state() : a32_state();
		for (unsigned index = 0; index < 13; ++index)
			cpu.registers[index] = data_base + 0x100 + 0x40 * index;
		cpu.registers[13] = data_base + 0x1000;
		cpu.registers[14] = code_base + 0x200;
		return cpu;
	}

	void finish(const decode::Instruction &instruction,
		    ::uml::cpu::arm64::CpuState cpu, std::uint32_t word)
	{
		const auto entry = cpu.pc;
		const auto result = guest_.run(cpu, budget);
		++checked_;
		/* UDF is a defined instruction whose effect is the exception. */
		const bool defined = instruction.status != decode::Status::undefined &&
			instruction.form != decode::Form::UDF_A1 &&
			instruction.form != decode::Form::UDF_T1 &&
			instruction.form != decode::Form::UDF_T2;
		if (defined)
			++accepted_;
		/* A defined branch may land on the word's own operand bytes,
		 * which are not code; that is illegal at a PC other than the
		 * word's, and the word itself ran. */
		const bool illegal = result.reason == ExitReason::illegal_instruction &&
			(!defined || cpu.pc == entry);
		if (defined == illegal || result.reason == ExitReason::fatal) {
			std::fprintf(stderr,
				"word %08x: the decoder says %s (reason %d, form %s) "
				"and the engine exits %d\n",
				word, defined ? "defined" : "undefined",
				static_cast<int>(instruction.reason),
				instruction.info().name,
				static_cast<int>(result.reason));
			std::abort();
		}
		if (!defined) {
			require(cpu.pc == entry, "an undefined word moved the PC");
			require(result.retired_instructions == 0,
				"an undefined word retired");
		}
	}

	Guest guest_;
	decode::Profile profile_;
	std::size_t checked_{};
	std::size_t accepted_{};
};

} // namespace

int main(int argc, char **argv)
{
	const ::uml::testing::RuntimeTestRoute runtime_route;
	require(argc == 2, "usage: forms_tests DECODER_DATA_DIRECTORY");
	Runner runner;
	std::set<std::pair<char, std::uint32_t>> seen;
	for (const auto &sample : read_samples(argv[1])) {
		if (!seen.emplace(sample.isa, sample.word).second)
			continue;
		if (sample.isa == 'A')
			runner.a32(sample.word);
		else
			runner.thumb(sample.word);
	}
	/* Every 16-bit Thumb instruction. */
	for (std::uint32_t halfword = 0; halfword < 0xe800; ++halfword)
		runner.thumb(halfword);
	std::printf("arm64-gadget-a32-forms: %zu words, %zu accepted\n",
		runner.checked(), runner.accepted());
	return 0;
}
