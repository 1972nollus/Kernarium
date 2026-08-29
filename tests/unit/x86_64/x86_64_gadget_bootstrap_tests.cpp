/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/decoder.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#include "memory/guest_address_space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>

namespace {

class TestMemory final : public uml::memory::GuestAddressSpace {
public:
	void load(std::uint64_t address,
		std::initializer_list<std::uint8_t> bytes) noexcept
	{
		for (const auto byte : bytes)
			bytes_[address++] = static_cast<std::byte>(byte);
	}

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		if (address > bytes_.size() ||
			destination.size() > bytes_.size() - address) {
			fault_ = {address, uml::memory::MemoryAccess::read};
			faulted_ = true;
			return false;
		}
		std::memcpy(destination.data(), bytes_.data() + address,
			destination.size());
		return true;
	}

	[[nodiscard]] bool fetch(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		if (!read(address, destination)) {
			fault_.access = uml::memory::MemoryAccess::execute;
			return false;
		}
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> source) override
	{
		if (address > bytes_.size() || source.size() > bytes_.size() - address) {
			fault_ = {address, uml::memory::MemoryAccess::write};
			faulted_ = true;
			return false;
		}
		std::memcpy(bytes_.data() + address, source.data(), source.size());
		return true;
	}

	[[nodiscard]] uml::memory::MemoryFault last_fault() const noexcept override
	{
		return fault_;
	}

	[[nodiscard]] bool has_fault() const noexcept override { return faulted_; }

	[[nodiscard]] std::uint64_t qword(std::uint64_t address) const noexcept
	{
		std::uint64_t value{};
		std::memcpy(&value, bytes_.data() + address, sizeof(value));
		return value;
	}

private:
	std::array<std::byte, 4096> bytes_{};
	uml::memory::MemoryFault fault_{};
	bool faulted_{};
};

[[nodiscard]] bool expect(bool condition, const char *message) noexcept
{
	if (!condition)
		std::fprintf(stderr, "x86-64 gadget bootstrap: %s\n", message);
	return condition;
}

[[nodiscard]] bool decode_scaled_address()
{
	TestMemory memory;
	memory.load(0, {0x48, 0x8b, 0x44, 0x8d, 0xf0});
	const auto result = uml::cpu::x86_64::decode(memory, 0);
	const auto &source = result.instruction.source;
	return expect(result.status == uml::cpu::x86_64::DecodeStatus::decoded,
		"scaled load did not decode") &&
		expect(result.instruction.length == 5, "scaled load length is wrong") &&
		expect(source.kind == uml::cpu::x86_64::OperandKind::memory,
			"scaled load source is not memory") &&
		expect(source.memory.base == 5 && source.memory.index == 1 &&
			source.memory.scale == 4 && source.memory.displacement == -16,
			"ModR/M plus SIB address was not normalized");
}

[[nodiscard]] bool execute_scalar_control_flow()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x48, 0xb8, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x48, 0x83, 0xc0, 0x03,
		0x48, 0x83, 0xf8, 0x08,
		0x74, 0x07,
		0x48, 0xc7, 0xc7, 0x00, 0x00, 0x00, 0x00,
		0x48, 0xc7, 0xc7, 0x2a, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	CpuState cpu{};
	const auto first = gadgets::build(memory, 0);
	if (!expect(first.status == gadgets::BuildStatus::built,
		"scalar block did not build"))
		return false;
	const auto first_run = gadgets::execute(first.program, cpu, memory, 64);
	if (!expect(first_run.event == uml::cpu::EngineEvent::continued,
		"conditional block returned the wrong event") ||
		!expect(first_run.retired_instructions == 4,
			"conditional block retirement is wrong") ||
		!expect(cpu.gpr(Register::rax) == 8 && cpu.rip == 27,
			"arithmetic or equal branch result is wrong"))
		return false;

	const auto second = gadgets::build(memory, cpu.rip);
	if (!expect(second.status == gadgets::BuildStatus::built,
		"syscall block did not build"))
		return false;
	const auto second_run = gadgets::execute(second.program, cpu, memory, 64);
	return expect(second_run.event == uml::cpu::EngineEvent::system_call,
		"SYSCALL did not exit the gadget stream") &&
		expect(second_run.retired_instructions == 2,
			"SYSCALL block retirement is wrong") &&
		expect(cpu.gpr(Register::rdi) == 42 && cpu.rip == 36,
			"SYSCALL state is wrong") &&
		expect(cpu.gpr(Register::rcx) == 36 &&
			cpu.gpr(Register::r11) == cpu.rflags,
			"SYSCALL did not publish RCX/R11");
}

[[nodiscard]] bool execute_segment_memory()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x64, 0x48, 0x89, 0x43, 0x08,
		0x64, 0x48, 0x8b, 0x4b, 0x08,
	});
	CpuState cpu{};
	cpu.fs_base = 64;
	cpu.gpr(Register::rbx) = 16;
	cpu.gpr(Register::rax) = 0x1122'3344'5566'7788ULL;
	const auto built = gadgets::build(memory, 0, 2);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"FS memory block did not build"))
		return false;
	const auto result = gadgets::execute(built.program, cpu, memory, 2);
	return expect(result.event == uml::cpu::EngineEvent::continued,
		"FS memory block returned the wrong event") &&
		expect(result.retired_instructions == 2,
			"FS memory retirement is wrong") &&
		expect(memory.qword(88) == 0x1122'3344'5566'7788ULL &&
			cpu.gpr(Register::rcx) == 0x1122'3344'5566'7788ULL,
			"FS base was not applied to memory operands");
}

[[nodiscard]] bool execute_call_and_return()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xe8, 0x05, 0x00, 0x00, 0x00});
	memory.load(10, {0xc3});
	CpuState cpu{};
	cpu.gpr(Register::rsp) = 400;
	const auto caller = gadgets::build(memory, 0);
	if (!expect(caller.status == gadgets::BuildStatus::built,
		"CALL did not build"))
		return false;
	const auto call_result = gadgets::execute(caller.program, cpu, memory, 1);
	if (!expect(call_result.event == uml::cpu::EngineEvent::continued &&
		cpu.rip == 10 && cpu.gpr(Register::rsp) == 392 &&
		memory.qword(392) == 5,
		"CALL did not push its return address"))
		return false;
	const auto callee = gadgets::build(memory, cpu.rip);
	if (!expect(callee.status == gadgets::BuildStatus::built,
		"RET did not build"))
		return false;
	const auto return_result = gadgets::execute(
		callee.program, cpu, memory, 1);
	return expect(return_result.event == uml::cpu::EngineEvent::continued &&
		cpu.rip == 5 && cpu.gpr(Register::rsp) == 400,
		"RET did not restore RIP/RSP");
}

[[nodiscard]] bool preserve_precise_memory_fault()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x48, 0x8b, 0x03});
	CpuState cpu{};
	cpu.gpr(Register::rbx) = 4095;
	const auto built = gadgets::build(memory, 0, 1);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"faulting load did not build"))
		return false;
	const auto result = gadgets::execute(built.program, cpu, memory, 1);
	return expect(result.event == uml::cpu::EngineEvent::memory_fault,
		"faulting load did not exit as a memory fault") &&
		expect(result.fault.address == 4095 &&
			result.fault.access == uml::memory::MemoryAccess::read,
			"faulting load lost precise metadata") &&
		expect(result.retired_instructions == 0 && cpu.rip == 0,
			"faulting load retired or advanced RIP");
}

[[nodiscard]] bool retire_valid_prefix_before_decode_fault()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x2a, 0x00, 0x00, 0x00,
		0x0f, 0x0b,
	});
	CpuState cpu{};
	const auto prefix = gadgets::build(memory, 0);
	if (!expect(prefix.status == gadgets::BuildStatus::built,
		"valid prefix before UD2 did not build"))
		return false;
	const auto prefix_result = gadgets::execute(
		prefix.program, cpu, memory, 64);
	if (!expect(prefix_result.event == uml::cpu::EngineEvent::continued &&
		prefix_result.retired_instructions == 1 && cpu.rip == 5 &&
		cpu.gpr(Register::rax) == 42,
		"valid prefix before UD2 did not retire first"))
		return false;
	const auto illegal = gadgets::build(memory, cpu.rip);
	return expect(illegal.status == gadgets::BuildStatus::illegal_instruction,
		"UD2 boundary was not reported after the valid prefix");
}

} // namespace

int main()
{
	const bool passed = decode_scaled_address() &&
		execute_scalar_control_flow() &&
		execute_segment_memory() &&
		execute_call_and_return() &&
		preserve_precise_memory_fault() &&
		retire_valid_prefix_before_decode_fault();
	return passed ? 0 : 1;
}
