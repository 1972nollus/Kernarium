// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_load_store_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace test = arm64_load_store_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x1b0'0000;
constexpr std::uint64_t base = 0x9a'0000;

struct LaneEncoding final {
	unsigned opcode;
	unsigned extra_register;
	bool full_vector;
	unsigned scale_bit;
	unsigned size;
	unsigned count;
	std::size_t element_size;
	std::size_t lane;
};

[[nodiscard]] bool decode_lane(unsigned opcode, unsigned extra_register,
	bool full_vector, unsigned scale_bit, unsigned size, LaneEncoding &result)
{
	const unsigned scale = opcode >> 1;
	if (scale >= 3U)
		return false;
	result = {opcode, extra_register, full_vector, scale_bit, size,
		((opcode & 1U) << 1) + extra_register + 1U, 0, 0};
	if (scale == 0U) {
		result.element_size = 1;
		result.lane = (static_cast<std::size_t>(full_vector) << 3) |
			(scale_bit << 2) | size;
		return true;
	}
	if (scale == 1U) {
		if ((size & 1U) != 0U)
			return false;
		result.element_size = 2;
		result.lane = (static_cast<std::size_t>(full_vector) << 2) |
			(scale_bit << 1) | (size >> 1);
		return true;
	}
	if ((size & 2U) != 0U || ((size & 1U) != 0U && scale_bit != 0U))
		return false;
	result.element_size = (size & 1U) != 0U ? 8 : 4;
	result.lane = (size & 1U) != 0U ?
		static_cast<std::size_t>(full_vector) :
		(static_cast<std::size_t>(full_vector) << 1) | scale_bit;
	return true;
}

[[nodiscard]] arm64::VectorRegister pattern(unsigned reg)
{
	arm64::VectorRegister value;
	for (unsigned index = 0; index < value.bytes.size(); ++index)
		value.bytes[index] = static_cast<std::byte>(reg * 29U + index * 7U + 9U);
	return value;
}

class LaneMemory final : public arm64::GuestMemory {
public:
	LaneMemory()
	{
		for (unsigned index = 0; index < payload.size(); ++index)
			payload[index] = static_cast<std::byte>(index * 19U + 0x31U);
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

	std::array<std::byte, 32> payload{};
	std::array<std::byte, 32> written{};
	std::uint64_t last_address{};
	std::size_t last_size{};
	unsigned reads{};
	unsigned writes{};
	bool fault{};
};

[[nodiscard]] std::array<arm64::VectorRegister, 32> expected_load(
	const std::array<arm64::VectorRegister, 32> &before,
	const LaneMemory &memory, const LaneEncoding &encoding,
	unsigned first_register)
{
	auto expected = before;
	const auto lane_offset = encoding.lane * encoding.element_size;
	for (unsigned reg = 0; reg < encoding.count; ++reg)
		std::copy_n(memory.payload.begin() + reg * encoding.element_size,
			encoding.element_size,
			expected[(first_register + reg) & 31U].bytes.begin() + lane_offset);
	return expected;
}

[[nodiscard]] std::array<std::byte, 32> expected_store(
	const std::array<arm64::VectorRegister, 32> &before,
	const LaneEncoding &encoding, unsigned first_register)
{
	std::array<std::byte, 32> expected{};
	const auto lane_offset = encoding.lane * encoding.element_size;
	for (unsigned reg = 0; reg < encoding.count; ++reg)
		std::copy_n(before[(first_register + reg) & 31U].bytes.begin() +
			lane_offset, encoding.element_size,
			expected.begin() + reg * encoding.element_size);
	return expected;
}

void require_vectors(const std::array<arm64::VectorRegister, 32> &actual,
	const std::array<arm64::VectorRegister, 32> &expected)
{
	test::require(std::memcmp(actual.data(), expected.data(),
		sizeof(actual)) == 0);
}

void check_interpreted(const LaneEncoding &encoding, bool load,
	bool post_index, unsigned offset_register, unsigned first_register)
{
	LaneMemory memory;
	arm64::CpuState cpu{};
	test::fill_registers(cpu);
	cpu.pc = pc;
	cpu.registers[2] = base;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		cpu.vectors[reg] = pattern(reg);
	if (offset_register != 31U)
		cpu.registers[offset_register] = 0x147;
	const auto before = cpu;
	const auto transfer_size = encoding.element_size * encoding.count;
	const auto instruction = test::simd_single_structure(
		encoding.full_vector, load, post_index, encoding.extra_register,
		offset_register, encoding.opcode, encoding.scale_bit, encoding.size,
		2, first_register);
	test::require(arm64::Dispatcher{}.execute(cpu, memory, instruction).reason ==
		arm64::StopReason::continued);
	test::require(cpu.pc == pc + 4);
	const auto offset = !post_index ? 0U :
		offset_register == 31U ? transfer_size : 0x147U;
	test::require(cpu.registers[2] == base + offset);
	test::require(memory.last_address == base && memory.last_size == transfer_size);
	if (load) {
		test::require(memory.reads == 1 && memory.writes == 0);
		require_vectors(cpu.vectors, expected_load(before.vectors, memory,
			encoding, first_register));
	} else {
		test::require(memory.reads == 0 && memory.writes == 1);
		const auto expected = expected_store(before.vectors, encoding,
			first_register);
		test::require(std::memcmp(memory.written.data(), expected.data(),
			transfer_size) == 0);
		require_vectors(cpu.vectors, before.vectors);
	}
}

template<typename Callback>
void for_each_lane_encoding(Callback callback)
{
	for (unsigned opcode = 0; opcode < 6; ++opcode)
		for (unsigned extra = 0; extra < 2; ++extra)
			for (const bool full : {false, true})
				for (unsigned scale_bit = 0; scale_bit < 2; ++scale_bit)
					for (unsigned size = 0; size < 4; ++size) {
						LaneEncoding encoding{};
						if (decode_lane(opcode, extra, full, scale_bit,
							size, encoding))
							callback(encoding);
					}
}

void exhaustive_interpreter_encodings()
{
	for_each_lane_encoding([](const LaneEncoding &encoding) {
		for (const bool load : {false, true}) {
			check_interpreted(encoding, load, false, 0, 30);
			check_interpreted(encoding, load, true, 31, 30);
			check_interpreted(encoding, load, true, 5, 30);
		}
	});
}

void special_registers_faults_and_base_offset_overlap()
{
	for_each_lane_encoding([](const LaneEncoding &encoding) {
		if (encoding.lane != 0)
			return;
		for (const bool load : {false, true}) {
			LaneMemory memory;
			arm64::CpuState cpu{};
			cpu.pc = pc;
			cpu.sp = base;
			for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
				cpu.vectors[reg] = pattern(reg);
			const auto before = cpu;
			const auto instruction = test::simd_single_structure(
				encoding.full_vector, load, true, encoding.extra_register,
				31, encoding.opcode, encoding.scale_bit, encoding.size,
				31, 31);
			test::require(arm64::Dispatcher{}.execute(cpu, memory,
				instruction).reason == arm64::StopReason::continued);
			test::require(cpu.sp == base +
				encoding.element_size * encoding.count);
			if (load)
				require_vectors(cpu.vectors, expected_load(before.vectors,
					memory, encoding, 31));

			memory = {};
			memory.fault = true;
			cpu = before;
			cpu.pc = pc;
			cpu.registers[2] = base;
			const auto fault_before = cpu;
			const auto result = arm64::Dispatcher{}.execute(cpu, memory,
				test::simd_single_structure(encoding.full_vector, load, true,
					encoding.extra_register, 31, encoding.opcode,
					encoding.scale_bit, encoding.size, 2, 30));
			test::require(result.reason == arm64::StopReason::memory_fault);
			test::require(result.write == !load && cpu.pc == pc);
			test::require(cpu.registers == fault_before.registers);
			require_vectors(cpu.vectors, fault_before.vectors);
		}
	});

	LaneEncoding encoding{};
	test::require(decode_lane(0, 0, false, 0, 0, encoding));
	LaneMemory memory;
	arm64::CpuState cpu{};
	cpu.pc = pc;
	cpu.registers[2] = base;
	test::require(arm64::Dispatcher{}.execute(cpu, memory,
		test::simd_single_structure(false, true, true, 0,
			2, 0, 0, 0, 2, 0)).reason == arm64::StopReason::continued);
	test::require(cpu.registers[2] == base + base);
}

void reserved_encodings_are_illegal()
{
	for (unsigned opcode = 0; opcode < 6; ++opcode)
		for (unsigned extra = 0; extra < 2; ++extra)
			for (const bool full : {false, true})
				for (unsigned scale_bit = 0; scale_bit < 2; ++scale_bit)
					for (unsigned size = 0; size < 4; ++size) {
						LaneEncoding encoding{};
						if (decode_lane(opcode, extra, full, scale_bit,
							size, encoding))
							continue;
						for (const bool load : {false, true}) {
							LaneMemory memory;
							arm64::CpuState cpu{};
							cpu.pc = pc;
							test::require(arm64::Dispatcher{}.execute(cpu, memory,
								test::simd_single_structure(full, load, false,
									extra, 0, opcode, scale_bit, size, 2, 3)).reason ==
								arm64::StopReason::illegal_instruction);
							test::require(memory.reads == 0 && memory.writes == 0);
						}
					}

	for_each_lane_encoding([](const LaneEncoding &encoding) {
		for (unsigned offset_register = 1; offset_register < 32;
		     ++offset_register) {
			LaneMemory memory;
			arm64::CpuState cpu{};
			cpu.pc = pc;
			const auto instruction = test::simd_single_structure(
				encoding.full_vector, true, false, encoding.extra_register,
				offset_register, encoding.opcode, encoding.scale_bit,
				encoding.size, 2, 3);
			/* FEAT_LRCPC3 allocates LDAP1/STL1 inside this formerly
			 * reserved single-structure space and has its own exhaustive
			 * allocation/behavior suites. */
			if ((instruction & 0xbf3f'fc00U) == 0x0d01'8400U)
				continue;
			const auto result = arm64::Dispatcher{}.execute(
				cpu, memory, instruction);
			if (result.reason != arm64::StopReason::illegal_instruction)
				std::fprintf(stderr,
					"reserved SIMD single-structure 0x%08x returned %u\n",
					instruction, static_cast<unsigned>(result.reason));
			test::require(result.reason ==
				arm64::StopReason::illegal_instruction);
		}
	});
}

void gadget_matrix()
{
	test::StaticMapping mapping{0x1d0'0000, 101,
		"/private/tmp/uml-simd-single.XXXXXX"};
	const auto map_base = mapping.guest() +
		5 * test::StaticMapping::page_size;
	for_each_lane_encoding([&](const LaneEncoding &encoding) {
		for (const bool load : {false, true})
			for (const unsigned offset_register : {0U, 31U, 5U}) {
				const bool post = offset_register != 0U;
				LaneMemory payload;
				arm64::CpuState cpu{};
				cpu.registers[2] = map_base;
				cpu.registers[5] = 0x147;
				for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
					cpu.vectors[reg] = pattern(reg);
				const auto before = cpu;
				const auto transfer_size = encoding.element_size * encoding.count;
				if (load)
					std::memcpy(mapping.at(map_base), payload.payload.data(),
						transfer_size);
				test::require(mapping.execute(pc,
					test::simd_single_structure(encoding.full_vector, load,
						post, encoding.extra_register, offset_register,
						encoding.opcode, encoding.scale_bit, encoding.size,
						2, 30), cpu) == gadgets::Event::system_call);
				if (load) {
					require_vectors(cpu.vectors, expected_load(before.vectors,
						payload, encoding, 30));
				} else {
					const auto expected = expected_store(before.vectors,
						encoding, 30);
					test::require(std::memcmp(mapping.at(map_base), expected.data(),
						transfer_size) == 0);
				}
				const auto offset = !post ? 0U : offset_register == 31U ?
					transfer_size : 0x147U;
				test::require(cpu.registers[2] == map_base + offset);
			}
	});
}

void gadget_edges_faults_and_illegal()
{
	test::StaticMapping mapping{0x1f0'0000, 103,
		"/private/tmp/uml-simd-single-edge.XXXXXX"};
	for (unsigned count = 1; count <= 4; ++count)
		for (unsigned element_class = 0; element_class < 4; ++element_class)
			for (const bool load : {false, true}) {
				const unsigned scale = element_class < 2 ? element_class : 2U;
				const unsigned opcode = (scale << 1) | (count >= 3U);
				const unsigned extra = (count - 1U) & 1U;
				const unsigned size = element_class == 1 ? 2U :
					element_class == 3 ? 1U : 0U;
				const unsigned scale_bit = element_class == 3 ? 0U : 1U;
				LaneEncoding encoding{};
				if (!decode_lane(opcode, extra, true,
					scale_bit, size, encoding))
					continue;
				const auto transfer_size = encoding.element_size * count;
				const auto cross_inside = std::max<std::size_t>(
					1, transfer_size / 2);
				const auto cross = mapping.guest() +
					4 * test::StaticMapping::page_size - cross_inside;
				LaneMemory payload;
				arm64::CpuState cpu{};
				cpu.registers[2] = cross;
				for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
					cpu.vectors[reg] = pattern(reg);
				const auto before = cpu;
				if (load)
					std::memcpy(mapping.at(cross), payload.payload.data(),
						transfer_size);
				test::require(mapping.execute(pc,
					test::simd_single_structure(true, load, false, extra, 0,
						opcode, scale_bit, size, 2, 30), cpu) ==
					gadgets::Event::system_call);
				if (load)
					require_vectors(cpu.vectors, expected_load(before.vectors,
						payload, encoding, 30));

				const auto inside = transfer_size / 2;
				const auto fault = mapping.guest() +
					test::StaticMapping::length - inside;
				std::array<std::byte, 32> sentinel{};
				sentinel.fill(std::byte{0x5a});
				if (inside != 0)
					std::memcpy(mapping.at(fault), sentinel.data(), inside);
				cpu = before;
				cpu.registers[2] = fault;
				const auto fault_before = cpu;
				test::require(mapping.execute(pc,
					test::simd_single_structure(true, load, false, extra, 0,
						opcode, scale_bit, size, 2, 30), cpu) ==
					gadgets::Event::memory_fault);
				test::require(cpu.pc == pc);
				test::require(cpu.registers == fault_before.registers);
				require_vectors(cpu.vectors, fault_before.vectors);
				if (!load && inside != 0)
					test::require(std::memcmp(mapping.at(fault), sentinel.data(),
						inside) == 0);
			}

	arm64::CpuState cpu{};
	test::require(mapping.execute(pc, test::simd_single_structure(
		true, true, false, 0, 0, 2, 0, 1, 2, 3), cpu) ==
		gadgets::Event::illegal_instruction);
	test::require(mapping.execute(pc, test::simd_single_structure(
		true, true, false, 0, 1, 0, 0, 0, 2, 3), cpu) ==
		gadgets::Event::illegal_instruction);
}

} // namespace

int main()
{
	exhaustive_interpreter_encodings();
	special_registers_faults_and_base_offset_overlap();
	reserved_encodings_are_illegal();
	gadget_matrix();
	gadget_edges_faults_and_illegal();
}
