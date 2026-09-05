// SPDX-License-Identifier: GPL-2.0-or-later
#include "gadget_ceiling_ideal.hpp"
#include "gadget_ceiling_workload.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;

extern "C" void uml_arm64_gadget_interrupt();

namespace {

constexpr std::uint64_t body_pc = 0x1000;
constexpr std::size_t body_instruction_count = 18;

[[noreturn]] void fail(const char *message)
{
	std::fprintf(stderr, "ideal gadget ceiling: %s\n", message);
	std::abort();
}

[[nodiscard]] std::uintptr_t gadget_address(void (*gadget)()) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

class IdealWorkload final {
public:
	IdealWorkload()
		: finish_stream_{
			gadget_address(uml_arm64_gadget_interrupt),
			body_pc + body_instruction_count * sizeof(std::uint32_t),
		  },
		  loop_link_{.guest_pc = body_pc},
		  finish_link_{
			.stream = finish_stream_.data(),
			.instruction_count = 0,
			.guest_pc = body_pc +
				body_instruction_count * sizeof(std::uint32_t),
		  }
	{
		const auto begin = reinterpret_cast<std::uintptr_t>(
			uml_gadget_ceiling_body_begin);
		const auto end = reinterpret_cast<std::uintptr_t>(
			uml_gadget_ceiling_body_end);
		const auto bytes =
			body_instruction_count * sizeof(std::uint32_t);
		if (end < begin || begin % alignof(std::uint32_t) != 0 ||
		    end - begin != bytes)
			fail("native body is not exactly 18 instructions");

		std::array<std::uint32_t, body_instruction_count> instructions;
		std::memcpy(instructions.data(),
			reinterpret_cast<const void *>(begin), bytes);
		program_.emplace(gadgets::Builder{}.build(
			body_pc, instructions,
			[this](std::uint64_t pc) -> const gadgets::BlockLink * {
				if (pc == loop_link_.guest_pc)
					return &loop_link_;
				if (pc == finish_link_.guest_pc)
					return &finish_link_;
				fail("Builder requested an unexpected link target");
			}));
		if (!program_->complete ||
		    program_->program.instruction_count() !=
			    body_instruction_count)
			fail("production Builder did not translate the complete body");

		loop_link_.stream = program_->program.words().data();
		/* A zero link count is the benchmark's defining idealization. It keeps
		 * the production threaded stream inside gadget code instead of taking
		 * the ordinary 4,096-instruction fairness exit to the executor. */
		loop_link_.instruction_count = 0;
	}

	[[nodiscard]] std::uint64_t run(std::uint64_t iterations)
	{
		arm64::CpuState cpu{};
		cpu.registers[19] = iterations;
		cpu.registers[20] = 1;
		std::size_t executed = 0;
		const auto event = gadgets::execute(
			program_->program, cpu, tlb_, executed);
		if (event != ::uml::cpu::EngineEvent::interrupt)
			fail("stream did not reach its final gadget interrupt");
		if (cpu.registers[19] != 0)
			fail("stream left the loop before its final iteration");
		if (executed != body_instruction_count)
			fail("linked stream changed its no-exit retirement sentinel");
		return cpu.registers[20];
	}

private:
	std::array<std::uintptr_t, 2> finish_stream_;
	gadgets::BlockLink loop_link_;
	gadgets::BlockLink finish_link_;
	std::optional<gadgets::BuildResult> program_;
	::uml::memory::Tlb tlb_;
};

std::unique_ptr<IdealWorkload> ideal;

} // namespace

void uml_gadget_ceiling_prepare_ideal()
{
	ideal = std::make_unique<IdealWorkload>();
}

std::uint64_t uml_gadget_ceiling_ideal_workload(std::uint64_t iterations)
{
	if (!ideal)
		fail("workload was not prepared outside the timed region");
	return ideal->run(iterations);
}
