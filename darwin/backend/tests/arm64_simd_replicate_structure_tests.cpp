// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x210'0000;
constexpr std::uint64_t base = 0xa0'0000;

struct Encoding final {
	unsigned count;
	unsigned opcode;
	unsigned extra_register;
	bool full_vector;
	unsigned size;
	std::size_t element_size;
};

[[nodiscard]] Encoding encoding(unsigned count, bool full_vector, unsigned size)
{
	return {count, count <= 2 ? 6U : 7U, (count - 1U) & 1U,
		full_vector, size, std::size_t{1} << size};
}

[[nodiscard]] arm64::VectorRegister pattern(unsigned reg)
{
	arm64::VectorRegister value;
	for (unsigned index = 0; index < value.bytes.size(); ++index)
		value.bytes[index] = static_cast<std::byte>(reg * 31U + index * 9U + 7U);
	return value;
}

class ReplicateMemory final : public arm64::GuestMemory {
public:
	ReplicateMemory()
	{
		for (unsigned index = 0; index < payload.size(); ++index)
			payload[index] = static_cast<std::byte>(index * 23U + 0x41U);
	}

	[[nodiscard]] bool read(std::uint64_t address,
				std::span<std::byte> output) override
	{
		++reads;
		last_address = address;
		last_size = output.size();
		if (fault)
			return false;
		std::copy_n(payload.begin(), output.size(), output.begin());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
				 std::span<const std::byte>) override
	{
		++writes;
		last_address = address;
		return !fault;
	}

	std::array<std::byte, 32> payload{};
	std::uint64_t last_address{};
	std::size_t last_size{};
	unsigned reads{};
	unsigned writes{};
	bool fault{};
};

[[nodiscard]] std::array<arm64::VectorRegister, 32> expected_load(
	const std::array<arm64::VectorRegister, 32> &before,
	const ReplicateMemory &memory, const Encoding &form,
	unsigned first_register)
{
	auto expected = before;
	const std::size_t vector_size = form.full_vector ? 16 : 8;
	for (unsigned reg = 0; reg < form.count; ++reg) {
		auto &destination = expected[(first_register + reg) & 31U].bytes;
		destination.fill(std::byte{});
		for (std::size_t offset = 0; offset < vector_size;
		     offset += form.element_size)
			std::copy_n(memory.payload.begin() + reg * form.element_size,
				form.element_size, destination.begin() + offset);
	}
	return expected;
}

void require_vectors(const std::array<arm64::VectorRegister, 32> &actual,
	const std::array<arm64::VectorRegister, 32> &expected)
{
	test::require(std::memcmp(actual.data(), expected.data(),
		sizeof(actual)) == 0);
}

void check_interpreted(const Encoding &form, bool post_index,
	unsigned offset_register, unsigned first_register)
{
	ReplicateMemory memory;
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.pc = pc;
	cpu.registers[2] = base;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		cpu.vectors[reg] = pattern(reg);
	if (offset_register != 31U)
		cpu.registers[offset_register] = 0x159;
	const auto before = cpu;
	const auto transfer_size = form.element_size * form.count;
	const auto instruction = test::simd_single_structure(form.full_vector,
		true, post_index, form.extra_register, offset_register, form.opcode,
		0, form.size, 2, first_register);
	test::require(arm64::Dispatcher{}.execute(cpu, memory, instruction).reason ==
		arm64::StopReason::continued);
	test::require(cpu.pc == pc + 4);
	const auto offset = !post_index ? 0U : offset_register == 31U ?
		transfer_size : 0x159U;
	test::require(cpu.registers[2] == base + offset);
	test::require(memory.reads == 1 && memory.writes == 0);
	test::require(memory.last_address == base && memory.last_size == transfer_size);
	require_vectors(cpu.vectors,
		expected_load(before.vectors, memory, form, first_register));
}

void exhaustive_interpreter_encodings()
{
	for (unsigned count = 1; count <= 4; ++count)
		for (const bool full_vector : {false, true})
			for (unsigned size = 0; size < 4; ++size) {
				const auto form = encoding(count, full_vector, size);
				check_interpreted(form, false, 0, 30);
				check_interpreted(form, true, 31, 30);
				check_interpreted(form, true, 5, 30);
			}
}

void special_registers_and_faults()
{
	for (unsigned count = 1; count <= 4; ++count)
		for (unsigned size = 0; size < 4; ++size) {
			const auto form = encoding(count, true, size);
			ReplicateMemory memory;
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.sp = base;
			for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
				cpu.vectors[reg] = pattern(reg);
			const auto before = cpu;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				test::simd_single_structure(true, true, true,
					form.extra_register, 31, form.opcode, 0, size,
					31, 31)).reason == arm64::StopReason::continued);
			test::require(cpu.sp == base + form.element_size * count);
			require_vectors(cpu.vectors,
				expected_load(before.vectors, memory, form, 31));

			memory = {};
			memory.fault = true;
			cpu = before;
			cpu.pc = pc;
			cpu.registers[2] = base;
			const auto fault_before = cpu;
			const auto result = arm64::Dispatcher{}.execute(cpu, memory,
				test::simd_single_structure(true, true, true,
					form.extra_register, 31, form.opcode, 0, size,
					2, 30));
			test::require(result.reason == arm64::StopReason::memory_fault);
			test::require(!result.write && cpu.pc == pc);
			test::require(cpu.registers == fault_before.registers);
			require_vectors(cpu.vectors, fault_before.vectors);
		}

	ReplicateMemory memory;
	arm64::CpuState cpu{};
	cpu.pc = pc;
	cpu.registers[2] = base;
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		test::simd_single_structure(true, true, true, 0,
			2, 6, 0, 0, 2, 0)).reason == arm64::StopReason::continued);
	test::require(cpu.registers[2] == base + base);
}

void reserved_encodings_are_illegal()
{
	for (unsigned count = 1; count <= 4; ++count) {
		const unsigned opcode = count <= 2 ? 6U : 7U;
		const unsigned extra = (count - 1U) & 1U;
		for (const bool full : {false, true})
			for (unsigned size = 0; size < 4; ++size)
				for (const bool load : {false, true})
					for (const unsigned scale_bit : {0U, 1U}) {
						if (load && scale_bit == 0U)
							continue;
						ReplicateMemory memory;
						arm64::CpuState cpu{};
						cpu.pc = pc;
						test::require(arm64::Dispatcher{}.execute(cpu, memory,
							test::simd_single_structure(full, load, false,
								extra, 0, opcode, scale_bit, size, 2, 3)).reason ==
							arm64::StopReason::illegal_instruction);
						test::require(memory.reads == 0 && memory.writes == 0);
					}

		for (unsigned offset_register = 1; offset_register < 32;
		     ++offset_register) {
			ReplicateMemory memory;
			arm64::CpuState cpu{};
			cpu.pc = pc;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				test::simd_single_structure(true, true, false, extra,
					offset_register, opcode, 0, 0, 2, 3)).reason ==
				arm64::StopReason::illegal_instruction);
		}
	}
}

void gadget_matrix()
{
	test::StaticMapping mapping{0x230'0000, 107,
		"/private/tmp/uml-simd-replicate.XXXXXX"};
	const auto map_base = mapping.guest() +
		5 * test::StaticMapping::page_size;
	for (unsigned count = 1; count <= 4; ++count)
		for (const bool full : {false, true})
			for (unsigned size = 0; size < 4; ++size)
				for (const unsigned offset_register : {0U, 31U, 5U}) {
					const auto form = encoding(count, full, size);
					const bool post = offset_register != 0U;
					ReplicateMemory payload;
					arm64::CpuState cpu{};
					cpu.registers[2] = map_base;
					cpu.registers[5] = 0x159;
					for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
						cpu.vectors[reg] = pattern(reg);
					const auto before = cpu;
					const auto transfer_size = form.element_size * count;
					std::memcpy(mapping.at(map_base), payload.payload.data(),
						transfer_size);
					test::require(mapping.execute(pc,
						test::simd_single_structure(full, true, post,
							form.extra_register, offset_register, form.opcode,
							0, size, 2, 30), cpu) == gadgets::Event::system_call);
					require_vectors(cpu.vectors,
						expected_load(before.vectors, payload, form, 30));
					const auto offset = !post ? 0U : offset_register == 31U ?
						transfer_size : 0x159U;
					test::require(cpu.registers[2] == map_base + offset);
				}
}

void gadget_edges_faults_and_illegal()
{
	test::StaticMapping mapping{0x250'0000, 109,
		"/private/tmp/uml-simd-replicate-edge.XXXXXX"};
	for (unsigned count = 1; count <= 4; ++count)
		for (unsigned size = 0; size < 4; ++size) {
			const auto form = encoding(count, true, size);
			const auto transfer_size = form.element_size * count;
			const auto inside = std::max<std::size_t>(1, transfer_size / 2);
			const auto cross = mapping.guest() +
				4 * test::StaticMapping::page_size - inside;
			ReplicateMemory payload;
			arm64::CpuState cpu{};
			cpu.registers[2] = cross;
			for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
				cpu.vectors[reg] = pattern(reg);
			const auto before = cpu;
			std::memcpy(mapping.at(cross), payload.payload.data(), transfer_size);
			test::require(mapping.execute(pc, test::simd_single_structure(
				true, true, false, form.extra_register, 0,
				form.opcode, 0, size, 2, 30), cpu) ==
				gadgets::Event::system_call);
			require_vectors(cpu.vectors,
				expected_load(before.vectors, payload, form, 30));

			const auto mapped_bytes = transfer_size / 2;
			const auto fault = mapping.guest() + test::StaticMapping::length -
				mapped_bytes;
			cpu = before;
			cpu.registers[2] = fault;
			const auto fault_before = cpu;
			test::require(mapping.execute(pc, test::simd_single_structure(
				true, true, false, form.extra_register, 0,
				form.opcode, 0, size, 2, 30), cpu) ==
				gadgets::Event::memory_fault);
			test::require(cpu.pc == pc);
			test::require(cpu.registers == fault_before.registers);
			require_vectors(cpu.vectors, fault_before.vectors);
		}

	arm64::CpuState cpu{};
	test::require(mapping.execute(pc, test::simd_single_structure(
		true, false, false, 0, 0, 6, 0, 0, 2, 3), cpu) ==
		gadgets::Event::illegal_instruction);
	test::require(mapping.execute(pc, test::simd_single_structure(
		true, true, false, 0, 0, 6, 1, 0, 2, 3), cpu) ==
		gadgets::Event::illegal_instruction);
}

} // namespace

int main()
{
	exhaustive_interpreter_encodings();
	special_registers_and_faults();
	reserved_encodings_are_illegal();
	gadget_matrix();
	gadget_edges_faults_and_illegal();
}
