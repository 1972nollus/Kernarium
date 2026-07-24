// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <source_location>
#include <span>
#include <sys/mman.h>
#include <unistd.h>

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x6b0'0000;
constexpr std::uint64_t memory_base = 0x920'0000;
constexpr std::uint64_t source_address = memory_base + 0x17f;
constexpr std::uint64_t zero_base = memory_base + 0x140;
constexpr std::byte fill{0x5a};

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

class CheckedMemory final : public arm64::GuestMemory {
public:
	CheckedMemory() { bytes.fill(fill); }

	[[nodiscard]] bool read(
		std::uint64_t, std::span<std::byte>) override
	{
		++reads;
		return false;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> source) override
	{
		++writes;
		last_address = address;
		last_size = source.size();
		if (address < memory_base ||
		    address - memory_base > bytes.size() ||
		    source.size() > bytes.size() - (address - memory_base))
			return false;
		std::copy(source.begin(), source.end(),
			bytes.begin() + static_cast<std::ptrdiff_t>(
				address - memory_base));
		return true;
	}

	std::array<std::byte, 4096> bytes{};
	std::size_t reads{};
	std::size_t writes{};
	std::uint64_t last_address{};
	std::size_t last_size{};
};

void require_zeroed(const std::array<std::byte, 4096> &bytes)
{
	for (std::size_t index = 0; index < bytes.size(); ++index) {
		const auto guest_address = memory_base + index;
		const bool in_zero_block =
			guest_address >= zero_base && guest_address < zero_base + 64;
		require(bytes[index] == (in_zero_block ? std::byte{} : fill));
	}
}

[[nodiscard]] arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (std::size_t index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0x1234'0000'0000'0000ULL + index;
	cpu.pc = pc;
	return cpu;
}

void interpreter_zeroes_every_register_form()
{
	for (unsigned source = 0; source < 31; ++source) {
		CheckedMemory memory;
		auto cpu = state();
		cpu.registers[source] = source_address;
		const auto expected_registers = cpu.registers;
		const auto instruction = 0xd50b'7420U | source; // dc zva, Xn
		const auto result =
			arm64::Dispatcher{}.execute(cpu, memory, instruction);
		require(result.reason == arm64::StopReason::continued);
		require(cpu.pc == pc + 4);
		require(cpu.registers == expected_registers);
		require(memory.reads == 0 && memory.writes == 1);
		require(memory.last_address == zero_base && memory.last_size == 64);
		require_zeroed(memory.bytes);
	}
}

void interpreter_fault_is_atomic()
{
	CheckedMemory memory;
	auto cpu = state();
	const auto before = cpu;
	const auto result = arm64::Dispatcher{}.execute(
		cpu, memory, 0xd50b'743fU); // dc zva, xzr
	require(result.reason == arm64::StopReason::memory_fault);
	require(result.address == 0 && result.write);
	require(cpu.pc == before.pc && cpu.registers == before.registers);
	require(memory.reads == 0 && memory.writes == 1);
	require(std::ranges::all_of(memory.bytes,
		[](std::byte byte) { return byte == fill; }));
}

class TemporaryBacking final {
public:
	TemporaryBacking()
	{
		char path[] = "/private/tmp/uml-cache-zero.XXXXXX";
		descriptor_ = mkstemp(path);
		require(descriptor_ >= 0);
		require(unlink(path) == 0);
		require(ftruncate(descriptor_, 4096) == 0);
		std::array<std::byte, 4096> bytes{};
		bytes.fill(fill);
		require(pwrite(descriptor_, bytes.data(), bytes.size(), 0) ==
			static_cast<ssize_t>(bytes.size()));
	}

	~TemporaryBacking()
	{
		if (descriptor_ >= 0)
			close(descriptor_);
	}

	TemporaryBacking(const TemporaryBacking &) = delete;
	TemporaryBacking &operator=(const TemporaryBacking &) = delete;

	[[nodiscard]] int descriptor() const noexcept { return descriptor_; }

	[[nodiscard]] std::array<std::byte, 4096> read() const
	{
		std::array<std::byte, 4096> bytes{};
		require(pread(descriptor_, bytes.data(), bytes.size(), 0) ==
			static_cast<ssize_t>(bytes.size()));
		return bytes;
	}

private:
	int descriptor_{-1};
};

void gadget_zero_and_fault()
{
	constexpr unsigned long generation = 411;
	TemporaryBacking backing;
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(memory_base), backing.descriptor(), 0,
		4096, PROT_READ | PROT_WRITE, 0, 1) == 0);

	const std::array instructions{
		0xd50b'7427U, // dc zva, x7
		0xd400'0001U, // svc #0
	};
	const auto built = gadgets::Builder{}.build(pc, instructions);
	require(built.complete);
	auto cpu = state();
	cpu.registers[7] = source_address;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(generation, darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(built.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.pc == pc + 8);
	require_zeroed(backing.read());

	const std::array faulting{
		0xd50b'743fU, // dc zva, xzr
		0xd400'0001U, // svc #0
	};
	const auto fault_built = gadgets::Builder{}.build(pc, faulting);
	require(fault_built.complete);
	cpu = state();
	tlb->prepare(generation, darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(fault_built.program, cpu, *tlb) ==
		gadgets::Event::memory_fault);
	require(cpu.pc == pc);
	require(tlb->fault_address == 0 &&
		tlb->fault_access == gadgets::Access::write);

	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void production_specializes_source()
{
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	const auto reg = gadgets::Builder{}.build(pc,
		std::array{0xd50b'7420U, 0xd400'0001U});
	const auto zero = gadgets::Builder{}.build(pc,
		std::array{0xd50b'743fU, 0xd400'0001U});
	require(reg.complete && zero.complete);
	require(reg.program.words().front() != zero.program.words().front());
#endif
}

} // namespace

int main()
{
	interpreter_zeroes_every_register_form();
	interpreter_fault_is_atomic();
	gadget_zero_and_fault();
	production_specializes_source();
	return 0;
}
