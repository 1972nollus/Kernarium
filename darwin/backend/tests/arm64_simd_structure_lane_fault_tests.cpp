// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_structure_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>

namespace structure = arm64_simd_structure_test;
namespace engine = uml::backend::tests;
namespace arm64 = uml::backend::arm64;

namespace {

constexpr std::array modes{
	structure::AddressMode::no_offset,
	structure::AddressMode::post_immediate,
	structure::AddressMode::post_register,
};

struct LaneEncoding final {
	bool full_vector;
	bool load;
	unsigned extra;
	unsigned opcode;
	unsigned scale_bit;
	unsigned size;
	structure::SingleForm form;
};

template <typename Callback>
void for_each_lane_encoding(Callback callback)
{
	for (const bool full_vector : {false, true})
		for (const bool load : {false, true})
			for (unsigned extra = 0; extra < 2U; ++extra)
				for (unsigned opcode = 0; opcode < 6U; ++opcode)
					for (unsigned scale_bit = 0; scale_bit < 2U;
					     ++scale_bit)
						for (unsigned size = 0; size < 4U; ++size) {
							const auto form =
								structure::decode_single(
									full_vector, load, extra,
									opcode, scale_bit, size);
							if (form.valid)
								callback(LaneEncoding{
									full_vector, load, extra,
									opcode, scale_bit, size,
									form});
						}
}

[[nodiscard]] LaneEncoding representative(
	unsigned count, std::size_t width, bool load)
{
	LaneEncoding result{};
	bool found = false;
	for_each_lane_encoding([&](const LaneEncoding &encoding) {
		if (!found && encoding.full_vector && encoding.load == load &&
		    encoding.form.register_count == count &&
		    encoding.form.element_size == width) {
			result = encoding;
			found = true;
		}
	});
	if (!found)
		std::abort();
	return result;
}

[[nodiscard]] std::uint32_t instruction(
	const LaneEncoding &encoding, structure::AddressMode mode,
	unsigned rn = 2, unsigned rt = 29)
{
	return structure::single_instruction(
		encoding.full_vector, encoding.load,
		structure::post_index(mode), encoding.extra,
		structure::encoded_offset_register(mode, 5),
		encoding.opcode, encoding.scale_bit, encoding.size, rn, rt);
}

void cross_page_success(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(2U * structure::page_size);
	structure::fill_memory(memory, 0xeb);
	const auto boundary = structure::data_base + structure::page_size;
	for_each_lane_encoding([&](const LaneEncoding &encoding) {
		const auto transfer =
			encoding.form.register_count * encoding.form.element_size;
		const auto address = boundary -
			(transfer > 1U ? transfer / 2U : 0U);
		for (const auto mode : modes) {
			char name[160];
			std::snprintf(name, sizeof(name),
				"lane cross-page q=%u count=%u width=%zu lane=%zu "
				"load=%u mode=%u",
				static_cast<unsigned>(encoding.full_vector),
				encoding.form.register_count,
				encoding.form.element_size, encoding.form.lane,
				static_cast<unsigned>(encoding.load),
				static_cast<unsigned>(mode));
			structure::check_single_success(
				harness, name, encoding.form,
				encoding.full_vector, encoding.load, encoding.extra,
				encoding.opcode, encoding.scale_bit, encoding.size,
				mode, 2, 29, 5, address, memory, seed++);
		}
	});
}

void cross_page_faults_are_restartable(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0xfc);
	const auto boundary = structure::data_base + structure::page_size;
	for_each_lane_encoding([&](const LaneEncoding &encoding) {
		const auto transfer =
			encoding.form.register_count * encoding.form.element_size;
		const auto address = boundary -
			(transfer > 1U ? transfer / 2U : 0U);
		for (const auto mode : modes) {
			auto initial = structure::seeded_state(seed++);
			structure::set_base(initial, 2, address);
			char name[168];
			std::snprintf(name, sizeof(name),
				"lane cross-page-fault q=%u count=%u width=%zu "
				"lane=%zu load=%u mode=%u",
				static_cast<unsigned>(encoding.full_vector),
				encoding.form.register_count,
				encoding.form.element_size, encoding.form.lane,
				static_cast<unsigned>(encoding.load),
				static_cast<unsigned>(mode));
			structure::check_memory(
				harness, name, instruction(encoding, mode),
				initial, initial, memory, {},
				engine::ExpectedEvent::memory_fault,
				{
					.address = boundary,
					.access = encoding.load ?
						arm64::MemoryAccess::read :
						arm64::MemoryAccess::write,
				},
				true);
		}
	});
}

void every_boundary_split(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x10d);
	const auto boundary = structure::data_base + structure::page_size;
	for (unsigned count = 1; count <= 4U; ++count)
		for (const auto width : {1U, 2U, 4U, 8U})
			for (const bool load : {false, true}) {
				const auto encoding = representative(count, width, load);
				const auto transfer = count * width;
				for (std::size_t split = 1; split < transfer; ++split) {
					auto initial = structure::seeded_state(seed++);
					structure::set_base(initial, 2, boundary - split);
					char name[152];
					std::snprintf(name, sizeof(name),
						"lane every-split count=%u width=%u "
						"split=%zu load=%u",
						count, width, split,
						static_cast<unsigned>(load));
					structure::check_memory(
						harness, name,
						instruction(
							encoding,
							structure::AddressMode::
								post_immediate),
						initial, initial, memory, {},
						engine::ExpectedEvent::memory_fault,
						{
							.address = boundary,
							.access = load ?
								arm64::MemoryAccess::read :
								arm64::MemoryAccess::write,
						},
						true);
				}
			}
}

void stack_alignment_precedes_memory(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x21e);
	const auto address =
		structure::data_base + structure::page_size / 2U + 8U;
	for_each_lane_encoding([&](const LaneEncoding &encoding) {
		for (const auto mode : modes) {
			auto initial = structure::seeded_state(seed++);
			initial.sp = address;
			char name[160];
			std::snprintf(name, sizeof(name),
				"lane misaligned-sp q=%u count=%u width=%zu "
				"load=%u mode=%u",
				static_cast<unsigned>(encoding.full_vector),
				encoding.form.register_count,
				encoding.form.element_size,
				static_cast<unsigned>(encoding.load),
				static_cast<unsigned>(mode));
			structure::check_memory(
				harness, name, instruction(encoding, mode, 31),
				initial, initial, memory, {},
				engine::ExpectedEvent::memory_fault,
				{
					.address = address,
					.access = encoding.load ?
						arm64::MemoryAccess::read :
						arm64::MemoryAccess::write,
				},
				true);
		}
	});
}

void every_stack_misalignment(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x21f);
	const auto aligned =
		structure::data_base + structure::page_size / 2U;
	for (unsigned misalignment = 1; misalignment < 16U; ++misalignment)
		for (const bool load : {false, true}) {
			const auto encoding = representative(4, 8, load);
			const auto address = aligned + misalignment;
			auto initial = structure::seeded_state(seed++);
			initial.sp = address;
			char name[128];
			std::snprintf(name, sizeof(name),
				"lane every-sp-misalignment=%u load=%u",
				misalignment, static_cast<unsigned>(load));
			structure::check_memory(
				harness, name,
				instruction(
					encoding, structure::AddressMode::no_offset,
					31),
				initial, initial, memory, {},
				engine::ExpectedEvent::memory_fault,
				{
					.address = address,
					.access = load ?
						arm64::MemoryAccess::read :
						arm64::MemoryAccess::write,
				},
				true);
		}
}

void permission_faults(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x220);
	const auto address =
		structure::data_base + structure::page_size / 2U;
	for (const bool load : {false, true}) {
		const auto encoding = representative(4, 8, load);
		auto initial = structure::seeded_state(seed++);
		structure::set_base(initial, 2, address);
		const std::array regions{
			engine::MemoryRegion{
				.address = structure::data_base,
				.initial = memory,
				.readable = !load,
				.writable = load,
			},
		};
		harness.check({
			.name = load ?
				"lane read-permission fault" :
				"lane write-permission fault",
			.instruction = instruction(
				encoding, structure::AddressMode::no_offset),
			.initial = initial,
			.expected = initial,
			.event = engine::ExpectedEvent::memory_fault,
			.native = structure::native_requirement(),
			.fault = {
				.address = address,
				.access = load ?
					arm64::MemoryAccess::read :
					arm64::MemoryAccess::write,
			},
			.compare_fault = true,
			.memory = std::span<const engine::MemoryRegion>{regions},
		});
	}
}

void first_access_fault_metadata(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	const auto address = structure::data_base;
	for (unsigned count = 1; count <= 4U; ++count)
		for (const auto width : {1U, 2U, 4U, 8U})
			for (const bool load : {false, true}) {
				const auto encoding = representative(count, width, load);
				for (const auto mode : modes) {
					auto initial = structure::seeded_state(seed++);
					structure::set_base(initial, 2, address);
					char name[144];
					std::snprintf(name, sizeof(name),
						"lane unmapped count=%u width=%u load=%u "
						"mode=%u",
						count, width,
						static_cast<unsigned>(load),
						static_cast<unsigned>(mode));
					harness.check({
						.name = name,
						.instruction =
							instruction(encoding, mode),
						.initial = initial,
						.expected = initial,
						.event = engine::ExpectedEvent::
							memory_fault,
						.native =
							structure::native_requirement(),
						.fault = {
							.address = address,
							.access = load ?
								arm64::MemoryAccess::read :
								arm64::MemoryAccess::write,
						},
						.compare_fault = true,
					});
				}
			}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	std::uint64_t seed = 0xa800'0000'0000'0000ULL;
	cross_page_success(harness, seed);
	cross_page_faults_are_restartable(harness, seed);
	every_boundary_split(harness, seed);
	stack_alignment_precedes_memory(harness, seed);
	every_stack_misalignment(harness, seed);
	permission_faults(harness, seed);
	first_access_fault_metadata(harness, seed);
	harness.print_summary("arm64-simd-structure-lane-fault");
	return 0;
}
