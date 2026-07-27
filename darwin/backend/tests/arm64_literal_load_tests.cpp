// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <source_location>
#include <sys/mman.h>
#include <unistd.h>

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

class ProbeMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t address,
				std::span<std::byte> output) override
	{
		last_address = address;
		last_size = output.size();
		++reads;
		if (fault)
			return false;
		std::copy_n(payload.begin(), output.size(), output.begin());
		return true;
	}
	[[nodiscard]] bool write(
		std::uint64_t, std::span<const std::byte>) override
	{
		++writes;
		return false;
	}

	std::array<std::byte, 16> payload{};
	std::uint64_t last_address{};
	std::size_t last_size{};
	std::size_t reads{};
	std::size_t writes{};
	bool fault{};
};

void require(bool condition,
	     const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

constexpr std::uint64_t base = 0x8'0000;

[[nodiscard]] constexpr std::int64_t literal_offset(unsigned encoded)
{
	const auto value = static_cast<std::uint64_t>(encoded) << 2;
	return static_cast<std::int64_t>(value << (64 - 21)) >> (64 - 21);
}

[[nodiscard]] constexpr std::uint32_t instruction(
	bool vector, unsigned operation, unsigned encoded_offset, unsigned rd)
{
	return 0x1800'0000U | (operation << 30) |
		(static_cast<unsigned>(vector) << 26) |
		((encoded_offset & 0x7ffffU) << 5) | rd;
}

[[nodiscard]] constexpr std::uint32_t instruction_for_address(
	bool vector, unsigned operation, std::uint64_t pc,
	std::uint64_t target, unsigned rd)
{
	const auto offset = static_cast<std::uint64_t>(target - pc);
	return instruction(vector, operation,
		static_cast<unsigned>((offset >> 2) & 0x7ffffU), rd);
}

[[nodiscard]] std::uint64_t payload64(const ProbeMemory &memory)
{
	std::uint64_t value;
	std::memcpy(&value, memory.payload.data(), sizeof(value));
	return value;
}

void exhaustive_integer_offsets()
{
	ProbeMemory memory;
	for (unsigned index = 0; index < memory.payload.size(); ++index)
		memory.payload[index] = static_cast<std::byte>(index * 17 + 3);
	const auto raw = payload64(memory);
	for (unsigned operation = 0; operation < 3; ++operation)
		for (unsigned encoded = 0; encoded < (1U << 19); ++encoded) {
			arm64::CpuState cpu{};
			cpu.pc = base;
			cpu.registers[7] = ~std::uint64_t{0};
			const auto encoded_instruction =
				instruction(false, operation, encoded, 7);
			require(arm64::Dispatcher{}.execute(
				cpu, memory, encoded_instruction).reason ==
				arm64::StopReason::continued);
			require(memory.last_address == static_cast<std::uint64_t>(
				static_cast<std::int64_t>(base) + literal_offset(encoded)));
			require(memory.last_size == (operation == 1 ? 8U : 4U));
			const auto expected = operation == 0 ? raw & 0xffff'ffffU :
				operation == 1 ? raw : static_cast<std::uint64_t>(
					static_cast<std::int64_t>(
						static_cast<std::int32_t>(raw)));
			require(cpu.registers[7] == expected);
			require(cpu.pc == base + 4);
		}
	require(memory.writes == 0);
}

void destinations_and_vector_variants()
{
	ProbeMemory memory;
	for (unsigned index = 0; index < memory.payload.size(); ++index)
		memory.payload[index] = static_cast<std::byte>(0xf0U - index * 7U);
	for (unsigned destination = 0; destination < 32; ++destination)
		for (unsigned operation = 0; operation < 3; ++operation) {
			arm64::CpuState cpu{};
			cpu.pc = base;
			for (auto &vector : cpu.vectors)
				vector.bytes.fill(std::byte{0xa5});
			const auto encoded = instruction(
				true, operation, 0, destination);
			require(arm64::Dispatcher{}.execute(
				cpu, memory, encoded).reason ==
				arm64::StopReason::continued);
			const std::size_t size = std::size_t{4} << operation;
			require(memory.last_size == size);
			for (unsigned byte = 0; byte < 16; ++byte)
				require(cpu.vectors[destination].bytes[byte] ==
					(byte < size ? memory.payload[byte] : std::byte{}));
		}

	for (unsigned encoded = 0; encoded < (1U << 19); ++encoded) {
		auto cpu = arm64::CpuState{};
		cpu.pc = base;
		const auto reads = memory.reads;
		require(arm64::Dispatcher{}.execute(
			cpu, memory, instruction(true, 3, encoded, 0)).reason ==
			arm64::StopReason::illegal_instruction);
		require(cpu.pc == base);
		require(memory.reads == reads);
	}

	for (unsigned operation = 0; operation < 3; ++operation) {
		auto cpu = arm64::CpuState{};
		cpu.pc = base;
		cpu.vectors[5].bytes.fill(std::byte{0x5a});
		memory.fault = true;
		const auto result = arm64::Dispatcher{}.execute(
			cpu, memory, instruction(true, operation, 1, 5));
		require(result.reason == arm64::StopReason::memory_fault);
		require(result.address == base + 4);
		require(cpu.pc == base);
		for (const auto byte : cpu.vectors[5].bytes)
			require(byte == std::byte{0x5a});
		memory.fault = false;
	}
}

void gadget_memory_paths()
{
	char path[] = "/private/tmp/uml-literal-load.XXXXXX";
	const int fd = mkstemp(path);
	require(fd >= 0);
	require(unlink(path) == 0);
	require(ftruncate(fd, 2 * DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	constexpr std::uint64_t guest = 0x20'0000;
	constexpr unsigned long generation = 29;
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(reinterpret_cast<void *>(guest), fd,
		0, 2 * DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);

	const std::uint32_t word = 0xf123'4567U;
	const std::uint64_t doubleword = 0x0123'4567'89ab'cdefULL;
	const std::int32_t signed_word = -0x1234'567;
	const std::array<std::byte, 4> scalar{
		std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
	const std::array<std::byte, 8> vector64{
		std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8},
		std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12}};
	std::array<std::byte, 16> vector128{};
	for (unsigned index = 0; index < vector128.size(); ++index)
		vector128[index] = static_cast<std::byte>(0x80U + index);
	require(pwrite(fd, &word, sizeof(word), 0x200) == sizeof(word));
	require(pwrite(fd, &doubleword, sizeof(doubleword), 0x208) ==
		static_cast<ssize_t>(sizeof(doubleword)));
	require(pwrite(fd, &signed_word, sizeof(signed_word), 0x210) ==
		static_cast<ssize_t>(sizeof(signed_word)));
	require(pwrite(fd, scalar.data(), scalar.size(), 0x218) ==
		static_cast<ssize_t>(scalar.size()));
	require(pwrite(fd, vector64.data(), vector64.size(), 0x220) ==
		static_cast<ssize_t>(vector64.size()));
	require(pwrite(fd, vector128.data(), vector128.size(), 4092) ==
		static_cast<ssize_t>(vector128.size()));

	constexpr std::uint64_t pc = guest + 0x100;
	std::array<std::uint32_t, 7> stream{};
	stream[0] = instruction_for_address(false, 0, pc, guest + 0x200, 0);
	stream[1] = instruction_for_address(false, 1, pc + 4, guest + 0x208, 1);
	stream[2] = instruction_for_address(false, 2, pc + 8, guest + 0x210, 2);
	stream[3] = instruction_for_address(true, 0, pc + 12, guest + 0x218, 3);
	stream[4] = instruction_for_address(true, 1, pc + 16, guest + 0x220, 4);
	stream[5] = instruction_for_address(true, 2, pc + 20, guest + 4092, 5);
	stream[6] = 0xd400'0001U;
	const auto built = gadgets::Builder{}.build(pc, stream);
	require(built.complete);
	require(built.program.accesses_fp_state());
	auto cpu = arm64::CpuState{};
	cpu.pc = pc;
	for (auto &vector : cpu.vectors)
		vector.bytes.fill(std::byte{0x5a});
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(generation, darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(built.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	require(cpu.pc == pc + stream.size() * 4);
	require(cpu.registers[0] == word);
	require(cpu.registers[1] == doubleword);
	require(cpu.registers[2] == static_cast<std::uint64_t>(
		static_cast<std::int64_t>(signed_word)));
	require(std::memcmp(cpu.vectors[3].bytes.data(), scalar.data(),
		scalar.size()) == 0);
	require(std::memcmp(cpu.vectors[4].bytes.data(), vector64.data(),
		vector64.size()) == 0);
	for (unsigned byte = scalar.size(); byte < 16; ++byte)
		require(cpu.vectors[3].bytes[byte] == std::byte{});
	for (unsigned byte = vector64.size(); byte < 16; ++byte)
		require(cpu.vectors[4].bytes[byte] == std::byte{});
	require(cpu.vectors[5].bytes == vector128);

	constexpr std::uint64_t fault_pc = guest + 0x300;
	const auto fault_instruction = instruction_for_address(
		true, 2, fault_pc,
		guest + 2 * DARWIN_UML_GUEST_PAGE_SIZE - 4, 7);
	const auto fault_program = gadgets::Builder{}.build(
		fault_pc, std::array{fault_instruction});
	cpu = {};
	cpu.pc = fault_pc;
	cpu.vectors[7].bytes.fill(std::byte{0x77});
	tlb->prepare(generation, darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(fault_program.program, cpu, *tlb) ==
		gadgets::Event::memory_fault);
	require(cpu.pc == fault_pc);
	for (const auto byte : cpu.vectors[7].bytes)
		require(byte == std::byte{0x77});
	require(close(fd) == 0);
}

} // namespace

int main()
{
	exhaustive_integer_offsets();
	destinations_and_vector_variants();
	gadget_memory_paths();
	return 0;
}
