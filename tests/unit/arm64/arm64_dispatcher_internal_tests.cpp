// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/detail/fp_state_access.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>
#include <vector>

namespace arm64 = uml::cpu::arm64;

namespace {

static_assert(arm64::detail::instruction_accesses_fp_state(
	0x3d80'0020U)); // STR Q0, [X1]
static_assert(arm64::detail::instruction_accesses_fp_state(
	0x3dc0'0022U)); // LDR Q2, [X1]
static_assert(arm64::detail::instruction_accesses_fp_state(
	0xd53b'4404U)); // MRS X4, FPCR
static_assert(arm64::detail::instruction_accesses_fp_state(
	0xd51b'4427U)); // MSR FPSR, X7
static_assert(!arm64::detail::instruction_accesses_fp_state(
	0xd503'201fU)); // NOP

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

class ProbeMemory final : public arm64::GuestMemory {
public:
	ProbeMemory(std::uint64_t base, std::size_t size)
		: base_{base}, bytes_(size)
	{
	}

	[[nodiscard]] bool read(
		std::uint64_t address, std::span<std::byte> destination) override
	{
		++reads_;
		return copy_out(address, destination);
	}

	[[nodiscard]] bool fetch(
		std::uint64_t address, std::span<std::byte> destination) override
	{
		++fetches_;
		return copy_out(address, destination);
	}

	[[nodiscard]] bool write(
		std::uint64_t address, std::span<const std::byte> source) override
	{
		auto destination = range(address, source.size());
		if (destination.empty() && !source.empty())
			return false;
		std::ranges::copy(source, destination.begin());
		return true;
	}

	void invalidate_code(std::uint64_t address) override
	{
		invalidations_.push_back(address);
	}

	void put32(std::uint64_t address, std::uint32_t instruction)
	{
		std::array<std::byte, sizeof(instruction)> encoded{};
		for (std::size_t index = 0; index < encoded.size(); ++index)
			encoded[index] =
				static_cast<std::byte>(instruction >> (index * 8));
		require(write(address, encoded));
	}

	[[nodiscard]] std::size_t fetches() const noexcept { return fetches_; }
	[[nodiscard]] std::size_t reads() const noexcept { return reads_; }

	[[nodiscard]] const std::vector<std::uint64_t> &invalidations() const
	{
		return invalidations_;
	}

private:
	[[nodiscard]] std::span<std::byte> range(
		std::uint64_t address, std::size_t size)
	{
		if (address < base_)
			return {};
		const auto offset = address - base_;
		if (offset > bytes_.size() || size > bytes_.size() - offset)
			return {};
		return std::span{bytes_}.subspan(
			static_cast<std::size_t>(offset), size);
	}

	[[nodiscard]] bool copy_out(
		std::uint64_t address, std::span<std::byte> destination)
	{
		const auto source = range(address, destination.size());
		if (source.empty() && !destination.empty())
			return false;
		std::ranges::copy(source, destination.begin());
		return true;
	}

	std::uint64_t base_;
	std::vector<std::byte> bytes_;
	std::vector<std::uint64_t> invalidations_;
	std::size_t fetches_{};
	std::size_t reads_{};
};

void budget_exhaustion_reports_completed_work()
{
	constexpr std::uint64_t base = 0x1000;
	constexpr std::uint32_t nop = 0xd503'201fU;
	ProbeMemory memory{base, 0x100};
	memory.put32(base, nop);
	memory.put32(base + 4, nop);
	memory.put32(base + 8, nop);
	arm64::CpuState cpu{};
	cpu.pc = base;

	const auto result = arm64::Dispatcher{}.run(cpu, memory, 2);
	require(result.reason == arm64::StopReason::budget_exhausted);
	require(result.address == base + 8);
	require(result.instruction == 0);
	require(result.instructions == 2);
	require(!result.write);
	require(cpu.pc == base + 8);
	require(memory.fetches() == 2);
	require(memory.reads() == 0);
}

void terminal_event_reports_instruction_and_total_count()
{
	constexpr std::uint64_t base = 0x2000;
	constexpr std::uint32_t nop = 0xd503'201fU;
	constexpr std::uint32_t system_call = 0xd400'0001U;
	ProbeMemory memory{base, 0x100};
	memory.put32(base, nop);
	memory.put32(base + 4, system_call);
	arm64::CpuState cpu{};
	cpu.pc = base;

	const auto result = arm64::Dispatcher{}.run(cpu, memory, 4);
	require(result.reason == arm64::StopReason::system_call);
	require(result.address == base + 4);
	require(result.instruction == system_call);
	require(result.instructions == 2);
	require(!result.write);
	require(cpu.pc == base + 8);
	require(memory.fetches() == 2);
	require(memory.reads() == 0);
}

void invalid_instruction_preserves_precise_diagnostic_state()
{
	constexpr std::uint64_t base = 0x3000;
	constexpr std::uint32_t nop = 0xd503'201fU;
	constexpr std::uint32_t undefined = 0x0000'beefU;
	ProbeMemory memory{base, 0x100};
	memory.put32(base, nop);
	memory.put32(base + 4, undefined);
	arm64::CpuState cpu{};
	cpu.pc = base;

	const auto result = arm64::Dispatcher{}.run(cpu, memory, 4);
	require(result.reason == arm64::StopReason::illegal_instruction);
	require(result.address == base + 4);
	require(result.instruction == undefined);
	require(result.instructions == 1);
	require(!result.write);
	require(cpu.pc == base + 4);
	require(memory.fetches() == 2);
	require(memory.reads() == 0);
}

void fetch_fault_preserves_precise_diagnostic_state()
{
	constexpr std::uint64_t base = 0x4000;
	ProbeMemory memory{base, 4};
	arm64::CpuState cpu{};
	cpu.pc = base + 4;

	const auto result = arm64::Dispatcher{}.run(cpu, memory, 1);
	require(result.reason == arm64::StopReason::memory_fault);
	require(result.address == base + 4);
	require(result.instruction == 0);
	require(result.instructions == 0);
	require(!result.write);
	require(cpu.pc == base + 4);
	require(memory.fetches() == 1);
	require(memory.reads() == 0);
}

void code_invalidation_is_forwarded_to_memory_adapter()
{
	constexpr std::uint64_t base = 0x5000;
	constexpr std::uint64_t target = 0x1234'5000;
	constexpr std::uint32_t invalidate_x2 = 0xd50b'7522U;
	ProbeMemory memory{base, 4};
	memory.put32(base, invalidate_x2);
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[2] = target;

	const auto result = arm64::Dispatcher{}.step(cpu, memory);
	require(result.reason == arm64::StopReason::continued);
	require(result.instructions == 1);
	require(cpu.pc == base + 4);
	require(memory.invalidations().size() == 1);
	require(memory.invalidations()[0] == target);
	require(memory.fetches() == 1);
	require(memory.reads() == 0);
}

void deferred_simd_load_store_uses_coherent_state()
{
	constexpr std::uint64_t base = 0x6000;
	constexpr std::uint64_t store_address = base + 0x80;
	constexpr std::uint64_t load_address = base + 0xa0;
	constexpr std::uint32_t store_q0_x1 = 0x3d80'0020U;
	constexpr std::uint32_t load_q2_x1 = 0x3dc0'0022U;
	constexpr std::uint32_t initial_fpcr = 0x0140'0301U;
	constexpr std::uint32_t initial_fpsr = 0x8800'0011U;
	ProbeMemory memory{base, 0x100};

	std::array<arm64::VectorRegister, 32> store_image{};
	for (std::size_t index = 0; index < 16; ++index)
		store_image[0].bytes[index] =
			static_cast<std::byte>(0x10U + index);
	store_image[9].bytes.fill(std::byte{0x9a});

	arm64::CpuState store_cpu{};
	store_cpu.pc = base;
	store_cpu.registers[1] = store_address;
	store_cpu.defer_fp_load(
		store_image.data(), initial_fpcr, initial_fpsr);
	const auto store_result = arm64::Dispatcher{}.execute(
		store_cpu, memory, store_q0_x1);
	require(store_result.reason == arm64::StopReason::continued);
	require(store_cpu.pc == base + 4);
	require(store_cpu.fp_loaded);
	require(store_cpu.fp_dirty);
	require(store_cpu.vectors[0].bytes == store_image[0].bytes);
	require(store_cpu.vectors[9].bytes == store_image[9].bytes);
	require(store_cpu.fpcr == initial_fpcr);
	require(store_cpu.fpsr == initial_fpsr);
	std::array<std::byte, 16> stored{};
	require(memory.read(store_address, stored));
	require(stored == store_image[0].bytes);

	std::array<std::byte, 16> loaded{};
	for (std::size_t index = 0; index < loaded.size(); ++index)
		loaded[index] = static_cast<std::byte>(0xe0U - index);
	require(memory.write(load_address, loaded));
	std::array<arm64::VectorRegister, 32> load_image{};
	load_image[2].bytes.fill(std::byte{0x22});
	load_image[9].bytes.fill(std::byte{0x9b});

	arm64::CpuState load_cpu{};
	load_cpu.pc = base + 4;
	load_cpu.registers[1] = load_address;
	load_cpu.defer_fp_load(load_image.data(), initial_fpcr, initial_fpsr);
	const auto load_result = arm64::Dispatcher{}.execute(
		load_cpu, memory, load_q2_x1);
	require(load_result.reason == arm64::StopReason::continued);
	require(load_cpu.pc == base + 8);
	require(load_cpu.fp_loaded);
	require(load_cpu.fp_dirty);
	require(load_cpu.vectors[2].bytes == loaded);
	require(load_cpu.vectors[9].bytes == load_image[9].bytes);
	require(load_cpu.fpcr == initial_fpcr);
	require(load_cpu.fpsr == initial_fpsr);
}

void deferred_fp_control_status_reads_and_writes_persist()
{
	constexpr std::uint64_t base = 0x7000;
	constexpr std::uint32_t read_fpcr_x4 = 0xd53b'4404U;
	constexpr std::uint32_t read_fpsr_x5 = 0xd53b'4425U;
	constexpr std::uint32_t write_fpcr_x6 = 0xd51b'4406U;
	constexpr std::uint32_t write_fpsr_x7 = 0xd51b'4427U;
	constexpr std::uint32_t read_fpcr_x8 = 0xd53b'4408U;
	constexpr std::uint32_t read_fpsr_x9 = 0xd53b'4429U;
	constexpr std::uint32_t fpcr_mask = 0x07f7'9f07U;
	constexpr std::uint32_t fpsr_mask = 0xf800'009fU;
	constexpr std::uint32_t initial_fpcr = 0x0540'1303U;
	constexpr std::uint32_t initial_fpsr = 0x8800'0091U;
	constexpr std::uint64_t requested_fpcr = 0xffff'ffff'ffff'ffffULL;
	constexpr std::uint64_t requested_fpsr = 0xa5a5'a5a5'ffff'ffffULL;
	ProbeMemory memory{base, 4};
	std::array<arm64::VectorRegister, 32> deferred_image{};
	deferred_image[11].bytes.fill(std::byte{0x5c});

	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.defer_fp_load(
		deferred_image.data(), initial_fpcr, initial_fpsr);

	const auto require_continued = [&](std::uint32_t instruction) {
		const auto prior_pc = cpu.pc;
		const auto result =
			arm64::Dispatcher{}.execute(cpu, memory, instruction);
		require(result.reason == arm64::StopReason::continued);
		require(cpu.pc == prior_pc + 4);
	};

	require_continued(read_fpcr_x4);
	require(cpu.fp_loaded);
	require(cpu.fp_dirty);
	require(cpu.registers[4] == (initial_fpcr & fpcr_mask));
	require(cpu.fpcr == initial_fpcr);
	require(cpu.fpsr == initial_fpsr);
	require(cpu.vectors[11].bytes == deferred_image[11].bytes);

	require_continued(read_fpsr_x5);
	require(cpu.registers[5] == (initial_fpsr & fpsr_mask));
	cpu.registers[6] = requested_fpcr;
	require_continued(write_fpcr_x6);
	require(cpu.fpcr == (requested_fpcr & fpcr_mask));
	cpu.registers[7] = requested_fpsr;
	require_continued(write_fpsr_x7);
	require(cpu.fpsr == (requested_fpsr & fpsr_mask));

	require_continued(read_fpcr_x8);
	require_continued(read_fpsr_x9);
	require(cpu.registers[8] == (requested_fpcr & fpcr_mask));
	require(cpu.registers[9] == (requested_fpsr & fpsr_mask));
	require(cpu.vectors[11].bytes == deferred_image[11].bytes);
}

} // namespace

int main()
{
	budget_exhaustion_reports_completed_work();
	terminal_event_reports_instruction_and_total_count();
	invalid_instruction_preserves_precise_diagnostic_state();
	fetch_fault_preserves_precise_diagnostic_state();
	code_invalidation_is_forwarded_to_memory_adapter();
	deferred_simd_load_store_uses_coherent_state();
	deferred_fp_control_status_reads_and_writes_persist();
	return 0;
}
