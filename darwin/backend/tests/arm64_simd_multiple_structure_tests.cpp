// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstring>

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x150'0000;
constexpr std::uint64_t base = 0x95'0000;

struct Variant final {
	unsigned opcode;
	unsigned register_count;
	bool interleaved;
};

constexpr std::array variants{
	Variant{7, 1, false}, Variant{10, 2, false},
	Variant{6, 3, false}, Variant{2, 4, false},
	Variant{8, 2, true}, Variant{4, 3, true}, Variant{0, 4, true},
};

[[nodiscard]] bool valid_arrangement(
	const Variant &variant, bool full_vector, unsigned size)
{
	return !variant.interleaved || full_vector || size != 3U;
}

[[nodiscard]] arm64::VectorRegister pattern(unsigned reg)
{
	arm64::VectorRegister value;
	for (unsigned index = 0; index < value.bytes.size(); ++index)
		value.bytes[index] = static_cast<std::byte>(reg * 37U + index * 11U + 3U);
	return value;
}

class StructureMemory final : public arm64::GuestMemory {
public:
	StructureMemory()
	{
		for (unsigned index = 0; index < payload.size(); ++index)
			payload[index] = static_cast<std::byte>(index * 13U + 5U);
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
				 std::span<const std::byte> input) override
	{
		++writes;
		last_address = address;
		last_size = input.size();
		if (fault)
			return false;
		written.fill(std::byte{});
		std::copy(input.begin(), input.end(), written.begin());
		return true;
	}

	std::array<std::byte, 64> payload{};
	std::array<std::byte, 64> written{};
	std::uint64_t last_address{};
	std::size_t last_size{};
	unsigned reads{};
	unsigned writes{};
	bool fault{};
};

void require_loaded_register(const arm64::VectorRegister &actual,
	const StructureMemory &memory, const Variant &variant,
	bool full_vector, unsigned size, unsigned register_index)
{
	const std::size_t vector_size = full_vector ? 16 : 8;
	const std::size_t element_size = std::size_t{1} << size;
	for (std::size_t byte = 0; byte < actual.bytes.size(); ++byte) {
		std::byte expected{};
		if (byte < vector_size) {
			if (!variant.interleaved) {
				expected = memory.payload[register_index * vector_size + byte];
			} else {
				const auto lane = byte / element_size;
				const auto lane_byte = byte % element_size;
				expected = memory.payload[(lane * variant.register_count +
					register_index) * element_size + lane_byte];
			}
		}
		test::require(actual.bytes[byte] == expected);
	}
}

[[nodiscard]] std::array<std::byte, 64> expected_store(
	const arm64::CpuState &before, const Variant &variant,
	bool full_vector, unsigned size, unsigned first_register)
{
	std::array<std::byte, 64> expected{};
	const std::size_t vector_size = full_vector ? 16 : 8;
	const std::size_t element_size = std::size_t{1} << size;
	for (unsigned reg = 0; reg < variant.register_count; ++reg) {
		const auto &source = before.vectors[(first_register + reg) & 31U].bytes;
		for (std::size_t byte = 0; byte < vector_size; ++byte) {
			std::size_t destination;
			if (!variant.interleaved) {
				destination = reg * vector_size + byte;
			} else {
				const auto lane = byte / element_size;
				const auto lane_byte = byte % element_size;
				destination = (lane * variant.register_count + reg) *
					element_size + lane_byte;
			}
			expected[destination] = source[byte];
		}
	}
	return expected;
}

void check_interpreted(const Variant &variant, bool full_vector,
	unsigned size, bool load, bool post_index, unsigned offset_register,
	unsigned first_register)
{
	StructureMemory memory;
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.pc = pc;
	cpu.registers[2] = base;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		cpu.vectors[reg] = pattern(reg);
	if (offset_register != 31U)
		cpu.registers[offset_register] = 0x183;
	const auto before = cpu;
	const auto vector_size = full_vector ? 16U : 8U;
	const auto transfer_size = vector_size * variant.register_count;
	const auto expected_offset = !post_index ? 0U :
		offset_register == 31U ? transfer_size : 0x183U;
	const auto instruction = test::simd_multiple_structure(full_vector, load,
		post_index, offset_register, variant.opcode, size,
		2, first_register);
	test::require(arm64::Dispatcher{}.execute(cpu, memory, instruction).reason ==
		arm64::StopReason::continued);
	test::require(cpu.pc == pc + 4);
	test::require(cpu.registers[2] == base + expected_offset);
	test::require(memory.last_address == base);
	test::require(memory.last_size == transfer_size);
	if (load) {
		test::require(memory.reads == 1 && memory.writes == 0);
		for (unsigned reg = 0; reg < variant.register_count; ++reg)
			require_loaded_register(
				cpu.vectors[(first_register + reg) & 31U], memory,
				variant, full_vector, size, reg);
	} else {
		test::require(memory.reads == 0 && memory.writes == 1);
		const auto expected = expected_store(before, variant, full_vector,
			size, first_register);
		test::require(std::memcmp(memory.written.data(), expected.data(),
			transfer_size) == 0);
		test::require(std::memcmp(cpu.vectors.data(), before.vectors.data(),
			sizeof(cpu.vectors)) == 0);
	}
}

void exhaustive_interpreter_encodings()
{
	for (const auto &variant : variants)
		for (const bool full_vector : {false, true})
			for (unsigned size = 0; size < 4; ++size) {
				if (!valid_arrangement(variant, full_vector, size))
					continue;
				for (const bool load : {false, true}) {
					check_interpreted(variant, full_vector, size, load,
						false, 0, 30);
					check_interpreted(variant, full_vector, size, load,
						true, 31, 30);
					check_interpreted(variant, full_vector, size, load,
						true, 5, 30);
				}
			}
}

void special_registers_and_faults()
{
	for (const auto &variant : variants)
		for (const bool load : {false, true}) {
			StructureMemory memory;
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.sp = base;
			for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
				cpu.vectors[reg] = pattern(reg);
			const auto before = cpu;
			const auto instruction = test::simd_multiple_structure(
				true, load, true, 31, variant.opcode, 2, 31, 31);
			test::require(arm64::Dispatcher{}.execute(
				cpu, memory, instruction).reason ==
				arm64::StopReason::continued);
			test::require(cpu.sp == base + 16 * variant.register_count);
			if (load) {
				for (unsigned reg = 0; reg < variant.register_count; ++reg)
					require_loaded_register(cpu.vectors[(31 + reg) & 31U],
						memory, variant, true, 2, reg);
			} else {
				const auto expected = expected_store(before, variant,
					true, 2, 31);
				test::require(std::memcmp(memory.written.data(), expected.data(),
					16 * variant.register_count) == 0);
			}

			memory = {};
			memory.fault = true;
			cpu = before;
			cpu.pc = pc;
			cpu.registers[2] = base;
			const auto fault_before = cpu;
			const auto result = arm64::Dispatcher{}.execute(cpu, memory,
				test::simd_multiple_structure(true, load, true, 31,
					variant.opcode, 2, 2, 30));
			test::require(result.reason == arm64::StopReason::memory_fault);
			test::require(result.write == !load);
			test::require(cpu.pc == pc);
			test::require(cpu.registers == fault_before.registers);
			test::require(std::memcmp(cpu.vectors.data(),
				fault_before.vectors.data(), sizeof(cpu.vectors)) == 0);
		}

	/* Rm==Rn uses the original base value as the register post-index. */
	StructureMemory memory;
	arm64::CpuState cpu{};
	cpu.pc = pc;
	cpu.registers[2] = base;
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		test::simd_multiple_structure(true, true, true, 2,
			7, 0, 2, 0)).reason == arm64::StopReason::continued);
	test::require(cpu.registers[2] == base + base);
}

void reserved_encodings_are_illegal()
{
	constexpr std::array invalid_opcodes{
		1U, 3U, 5U, 9U, 11U, 12U, 13U, 14U, 15U};
	for (const auto opcode : invalid_opcodes)
		for (const bool full_vector : {false, true})
			for (unsigned size = 0; size < 4; ++size)
				for (const bool load : {false, true}) {
					StructureMemory memory;
					arm64::CpuState cpu{};
					cpu.pc = pc;
					test::require(arm64::Dispatcher{}.execute(cpu, memory,
						test::simd_multiple_structure(full_vector, load,
							false, 0, opcode, size, 2, 3)).reason ==
						arm64::StopReason::illegal_instruction);
					test::require(memory.reads == 0 && memory.writes == 0);
				}

	for (const auto &variant : variants) {
		if (variant.interleaved)
			for (const bool load : {false, true}) {
				StructureMemory memory;
				arm64::CpuState cpu{};
				cpu.pc = pc;
				test::require(arm64::Dispatcher{}.execute(cpu, memory,
					test::simd_multiple_structure(false, load, false, 0,
						variant.opcode, 3, 2, 3)).reason ==
					arm64::StopReason::illegal_instruction);
			}
		for (unsigned offset_register = 1; offset_register < 32;
		     ++offset_register) {
			StructureMemory memory;
			arm64::CpuState cpu{};
			cpu.pc = pc;
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				test::simd_multiple_structure(true, true, false,
					offset_register, variant.opcode, 0, 2, 3)).reason ==
				arm64::StopReason::illegal_instruction);
		}
	}
}

void gadget_matrix()
{
	test::StaticMapping mapping{0x170'0000, 89,
		"/private/tmp/uml-simd-multiple.XXXXXX"};
	const auto map_base = mapping.guest() +
		6 * test::StaticMapping::page_size;
	for (const auto &variant : variants)
		for (const bool full_vector : {false, true})
			for (unsigned size = 0; size < 4; ++size) {
				if (!valid_arrangement(variant, full_vector, size))
					continue;
				for (const bool load : {false, true})
					for (const unsigned offset_register : {0U, 31U, 5U}) {
						const bool post_index = offset_register != 0U;
						StructureMemory payload;
						arm64::CpuState cpu{};
						cpu.registers[2] = map_base;
						cpu.registers[5] = 0x183;
						for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
							cpu.vectors[reg] = pattern(reg);
						const auto before = cpu;
						const auto transfer_size = (full_vector ? 16U : 8U) *
							variant.register_count;
						if (load)
							std::memcpy(mapping.at(map_base), payload.payload.data(),
								transfer_size);
						test::require(mapping.execute(pc,
							test::simd_multiple_structure(full_vector, load,
								post_index, offset_register, variant.opcode,
								size, 2, 30), cpu) == gadgets::Event::system_call);
						if (load) {
							for (unsigned reg = 0; reg < variant.register_count;
							     ++reg)
								require_loaded_register(
									cpu.vectors[(30 + reg) & 31U], payload,
									variant, full_vector, size, reg);
						} else {
							const auto expected = expected_store(before, variant,
								full_vector, size, 30);
							test::require(std::memcmp(mapping.at(map_base),
								expected.data(), transfer_size) == 0);
						}
						const auto offset = !post_index ? 0U :
							offset_register == 31U ? transfer_size : 0x183U;
						test::require(cpu.registers[2] == map_base + offset);
					}
			}
}

void gadget_page_edges_faults_and_illegal()
{
	test::StaticMapping mapping{0x190'0000, 97,
		"/private/tmp/uml-simd-multiple-edge.XXXXXX"};
	for (const auto &variant : variants)
		for (const bool load : {false, true}) {
			const auto transfer_size = 16U * variant.register_count;
			const auto cross = mapping.guest() +
				4 * test::StaticMapping::page_size - transfer_size / 2;
			StructureMemory payload;
			arm64::CpuState cpu{};
			cpu.registers[2] = cross;
			for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
				cpu.vectors[reg] = pattern(reg);
			const auto before = cpu;
			if (load)
				std::memcpy(mapping.at(cross), payload.payload.data(),
					transfer_size);
			test::require(mapping.execute(pc, test::simd_multiple_structure(
				true, load, false, 0, variant.opcode, 2, 2, 30), cpu) ==
				gadgets::Event::system_call);
			if (load) {
				for (unsigned reg = 0; reg < variant.register_count; ++reg)
					require_loaded_register(cpu.vectors[(30 + reg) & 31U],
						payload, variant, true, 2, reg);
			} else {
				const auto expected = expected_store(before, variant, true, 2, 30);
				test::require(std::memcmp(mapping.at(cross), expected.data(),
					transfer_size) == 0);
			}

			const auto fault = mapping.guest() + test::StaticMapping::length -
				transfer_size / 2;
			std::array<std::byte, 64> sentinel{};
			sentinel.fill(std::byte{0x5a});
			std::memcpy(mapping.at(fault), sentinel.data(), transfer_size / 2);
			cpu = before;
			cpu.registers[2] = fault;
			const auto fault_before = cpu;
			test::require(mapping.execute(pc, test::simd_multiple_structure(
				true, load, false, 0, variant.opcode, 2, 2, 30), cpu) ==
				gadgets::Event::memory_fault);
			test::require(cpu.pc == pc);
			test::require(cpu.registers == fault_before.registers);
			test::require(std::memcmp(cpu.vectors.data(),
				fault_before.vectors.data(), sizeof(cpu.vectors)) == 0);
			if (!load)
				test::require(std::memcmp(mapping.at(fault), sentinel.data(),
					transfer_size / 2) == 0);
		}

	arm64::CpuState cpu{};
	test::require(mapping.execute(pc, test::simd_multiple_structure(
		false, true, false, 0, 8, 3, 2, 3), cpu) ==
		gadgets::Event::illegal_instruction);
	test::require(cpu.pc == pc);
	test::require(mapping.execute(pc, test::simd_multiple_structure(
		true, true, false, 1, 7, 0, 2, 3), cpu) ==
		gadgets::Event::illegal_instruction);
}

} // namespace

int main()
{
	exhaustive_interpreter_encodings();
	special_registers_and_faults();
	reserved_encodings_are_illegal();
	gadget_matrix();
	gadget_page_edges_faults_and_illegal();
}
