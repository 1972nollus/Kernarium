// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* The guest the gadget engine's AArch32 unit tests run in: two code pages
 * (writable, so a test can change its code) and two data pages under a
 * generation of their own, the production AArch32 loop over the gadget
 * executor, and the state of a task about to enter at the code. */
#include "jit_test_support.hpp"

#include "memory/tlb.hpp"

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/engine_execution.hpp"
#include "cpu/arm64/gadgets/executor.hpp"
#include "static_mmu_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <span>
#include <vector>

namespace uml::emulator::tests::a32 {

namespace arm64 = ::uml::cpu::arm64;
namespace aarch32 = ::uml::cpu::arm64::aarch32;
namespace gadgets = ::uml::cpu::arm64::gadgets;
using ::uml::cpu::ExecutionRequest;
using ::uml::cpu::ExecutionResult;
using ::uml::cpu::ExitReason;
using ::uml::memory::StaticGuestMemory;
using ::uml::testing::require;

constexpr std::uint32_t page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::uint32_t code_base = 0x4f00'0000U;
constexpr std::uint32_t data_base = 0x5000'0000U;
constexpr std::uint32_t unmapped = 0x6000'0000U;
constexpr std::size_t data_pages = 2;

/* The guest of one test: two code pages (writable, so a test can change
 * its code) and two data pages, under a generation of their own. The page
 * after the data is unmapped. */
class Guest final {
public:
	explicit Guest(unsigned long generation)
		: generation_{generation}
		, code_{data_pages}
		, data_{data_pages}
	{
		require(darwin_uml_static_vm_activate(generation) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(std::uintptr_t{code_base}),
			code_.descriptor(), 0, data_pages * page,
			PROT_READ | PROT_WRITE | PROT_EXEC, 1, 1) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(std::uintptr_t{data_base}),
			data_.descriptor(), 0, data_pages * page,
			PROT_READ | PROT_WRITE, 0, 1) == 0);
	}

	~Guest() { require(darwin_uml_static_vm_destroy(generation_) == 0); }

	Guest(const Guest &) = delete;
	Guest &operator=(const Guest &) = delete;

	[[nodiscard]] unsigned long generation() const noexcept
	{
		return generation_;
	}
	[[nodiscard]] StaticGuestMemory &memory() noexcept { return memory_; }
	[[nodiscard]] gadgets::GadgetExecutor &executor() noexcept
	{
		return executor_;
	}

	/* Words or halfwords at a code offset. */
	void a32(std::uint32_t offset, std::initializer_list<std::uint32_t> words)
	{
		for (const auto word : words) {
			code_.put32(offset, word);
			offset += 4;
		}
	}
	void t32(std::uint32_t offset,
		 std::initializer_list<std::uint16_t> halfwords)
	{
		for (const auto halfword : halfwords) {
			code_.put(offset, std::as_bytes(std::span{&halfword, 1}));
			offset += 2;
		}
	}
	/* One more data page at `address`. */
	void map_data(std::uint32_t address)
	{
		extra_.push_back(std::make_unique<JitBacking>(1));
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(std::uintptr_t{address}),
			extra_.back()->descriptor(), 0, page,
			PROT_READ | PROT_WRITE, 0, 1) == 0);
	}
	void data32(std::uint32_t offset, std::uint32_t value)
	{
		data_.put32(offset, value);
	}
	[[nodiscard]] std::uint32_t data32(std::uint32_t offset) const
	{
		return data_.get32(offset);
	}

	/* Every word of the code pages. */
	void fill_code(std::uint32_t word)
	{
		std::vector<std::uint32_t> words(data_pages * page / 4, word);
		code_.put(0, std::as_bytes(std::span{words}));
	}

	/* Tells the engine the code changed, as the kernel's synchronization
	 * after a store to code does. */
	void code_changed(std::uint32_t address = code_base)
	{
		darwin_uml_static_vm_invalidate_code(address);
	}

	/* Changes code the way a guest store followed by the kernel's
	 * synchronization does. */
	void patch(std::uint32_t address, std::span<const std::byte> bytes)
	{
		void *destination = nullptr;
		require(darwin_uml_static_vm_translate(generation_, address,
			DARWIN_UML_STATIC_WRITE, &destination) ==
			DARWIN_UML_STATIC_TRANSLATED);
		std::memcpy(destination, bytes.data(), bytes.size());
		darwin_uml_static_vm_invalidate_code(address);
	}

	[[nodiscard]] ExecutionResult run(arm64::CpuState &cpu,
		std::size_t budget = 10'000, bool single_step = false)
	{
		ExecutionRequest request{
			.cpu = cpu,
			.memory = memory_,
			.translated_memory = &memory_,
			.generation = generation_,
			.instruction_budget = budget,
			.single_step = single_step,
		};
		return arm64::detail::run_translated_aarch32_blocks(executor_, request);
	}

private:
	unsigned long generation_;
	JitBacking code_;
	JitBacking data_;
	std::vector<std::unique_ptr<JitBacking>> extra_;
	StaticGuestMemory memory_;
	gadgets::GadgetExecutor executor_;
};

[[nodiscard]] arm64::CpuState a32_state(std::uint32_t pc = code_base)
{
	arm64::CpuState cpu{};
	cpu.a32 = 1;
	cpu.a32_cp15ben = 1;
	cpu.pc = aarch32::a32_key(pc);
	return cpu;
}

[[nodiscard]] arm64::CpuState t32_state(std::uint32_t pc = code_base)
{
	arm64::CpuState cpu = a32_state();
	cpu.pc = aarch32::t32_key(pc);
	return cpu;
}

[[nodiscard]] std::uint32_t reg(const arm64::CpuState &cpu, unsigned index)
{
	return static_cast<std::uint32_t>(cpu.registers[index]);
}

} // namespace uml::emulator::tests::a32
