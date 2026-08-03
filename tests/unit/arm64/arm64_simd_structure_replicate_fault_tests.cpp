// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_structure_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>

namespace structure = arm64_simd_structure_test;
namespace engine = uml::emulator::tests;
namespace arm64 = uml::cpu::arm64;

namespace {

constexpr std::array modes{
	structure::AddressMode::no_offset,
	structure::AddressMode::post_immediate,
	structure::AddressMode::post_register,
};

struct ReplicateEncoding final {
	bool full_vector;
	unsigned extra;
	unsigned opcode;
	unsigned size;
	structure::SingleForm form;
};

template <typename Callback>
void for_each_replicate_encoding(Callback callback)
{
	for (const bool full_vector : {false, true})
		for (unsigned extra = 0; extra < 2U; ++extra)
			for (unsigned opcode = 6; opcode < 8U; ++opcode)
				for (unsigned size = 0; size < 4U; ++size) {
					const auto form = structure::decode_single(
						full_vector, true, extra, opcode, 0, size);
					if (form.valid)
						callback(ReplicateEncoding{
							full_vector, extra, opcode, size,
							form});
				}
}

[[nodiscard]] ReplicateEncoding representative(
	unsigned count, std::size_t width)
{
	ReplicateEncoding result{};
	bool found = false;
	for_each_replicate_encoding([&](const ReplicateEncoding &encoding) {
		if (!found && encoding.full_vector &&
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
	const ReplicateEncoding &encoding, structure::AddressMode mode,
	unsigned rn = 2, unsigned rt = 29)
{
	return structure::single_instruction(
		encoding.full_vector, true, structure::post_index(mode),
		encoding.extra, structure::encoded_offset_register(mode, 5),
		encoding.opcode, 0, encoding.size, rn, rt);
}

void cross_page_success(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(2U * structure::page_size);
	structure::fill_memory(memory, 0x32f);
	const auto boundary = structure::data_base + structure::page_size;
	for_each_replicate_encoding(
		[&](const ReplicateEncoding &encoding) {
			const auto transfer =
				encoding.form.register_count *
				encoding.form.element_size;
			const auto address = boundary -
				(transfer > 1U ? transfer / 2U : 0U);
			for (const auto mode : modes) {
				char name[152];
				std::snprintf(name, sizeof(name),
					"replicate cross-page q=%u count=%u width=%zu "
					"mode=%u",
					static_cast<unsigned>(
						encoding.full_vector),
					encoding.form.register_count,
					encoding.form.element_size,
					static_cast<unsigned>(mode));
				structure::check_single_success(
					harness, name, encoding.form,
					encoding.full_vector, true, encoding.extra,
					encoding.opcode, 0, encoding.size, mode, 2,
					29, 5, address, memory, seed++);
			}
		});
}

void cross_page_faults_are_restartable(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x430);
	const auto boundary = structure::data_base + structure::page_size;
	for_each_replicate_encoding(
		[&](const ReplicateEncoding &encoding) {
			const auto transfer =
				encoding.form.register_count *
				encoding.form.element_size;
			const auto address = boundary -
				(transfer > 1U ? transfer / 2U : 0U);
			for (const auto mode : modes) {
				auto initial = structure::seeded_state(seed++);
				structure::set_base(initial, 2, address);
				char name[160];
				std::snprintf(name, sizeof(name),
					"replicate cross-page-fault q=%u count=%u "
					"width=%zu mode=%u",
					static_cast<unsigned>(
						encoding.full_vector),
					encoding.form.register_count,
					encoding.form.element_size,
					static_cast<unsigned>(mode));
				structure::check_memory(
					harness, name, instruction(encoding, mode),
					initial, initial, memory, {},
					engine::ExpectedEvent::memory_fault,
					{
						.address = boundary,
						.access =
							arm64::MemoryAccess::read,
					},
					true);
			}
		});
}

void every_boundary_split(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x541);
	const auto boundary = structure::data_base + structure::page_size;
	for (unsigned count = 1; count <= 4U; ++count)
		for (const auto width : {1U, 2U, 4U, 8U}) {
			const auto encoding = representative(count, width);
			const auto transfer = count * width;
			for (std::size_t split = 1; split < transfer; ++split) {
				auto initial = structure::seeded_state(seed++);
				structure::set_base(initial, 2, boundary - split);
				char name[144];
				std::snprintf(name, sizeof(name),
					"replicate every-split count=%u width=%u "
					"split=%zu",
					count, width, split);
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
						.access =
							arm64::MemoryAccess::read,
					},
					true);
			}
		}
}

void stack_alignment_precedes_memory(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x652);
	const auto address =
		structure::data_base + structure::page_size / 2U + 8U;
	for_each_replicate_encoding(
		[&](const ReplicateEncoding &encoding) {
			for (const auto mode : modes) {
				auto initial = structure::seeded_state(seed++);
				initial.sp = address;
				char name[152];
				std::snprintf(name, sizeof(name),
					"replicate misaligned-sp q=%u count=%u "
					"width=%zu mode=%u",
					static_cast<unsigned>(
						encoding.full_vector),
					encoding.form.register_count,
					encoding.form.element_size,
					static_cast<unsigned>(mode));
				structure::check_memory(
					harness, name,
					instruction(encoding, mode, 31),
					initial, initial, memory, {},
					engine::ExpectedEvent::memory_fault,
					{
						.address = address,
						.access =
							arm64::MemoryAccess::read,
					},
					true);
			}
		});
}

void every_stack_misalignment(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x653);
	const auto aligned =
		structure::data_base + structure::page_size / 2U;
	const auto encoding = representative(4, 8);
	for (unsigned misalignment = 1; misalignment < 16U; ++misalignment) {
		const auto address = aligned + misalignment;
		auto initial = structure::seeded_state(seed++);
		initial.sp = address;
		char name[120];
		std::snprintf(name, sizeof(name),
			"replicate every-sp-misalignment=%u", misalignment);
		structure::check_memory(
			harness, name,
			instruction(
				encoding, structure::AddressMode::no_offset, 31),
			initial, initial, memory, {},
			engine::ExpectedEvent::memory_fault,
			{
				.address = address,
				.access = arm64::MemoryAccess::read,
			},
			true);
	}
}

void permission_fault(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x654);
	const auto address =
		structure::data_base + structure::page_size / 2U;
	const auto encoding = representative(4, 8);
	auto initial = structure::seeded_state(seed++);
	structure::set_base(initial, 2, address);
	const std::array regions{
		engine::MemoryRegion{
			.address = structure::data_base,
			.initial = memory,
			.readable = false,
			.writable = true,
		},
	};
	harness.check({
		.name = "replicate read-permission fault",
		.instruction = instruction(
			encoding, structure::AddressMode::no_offset),
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::memory_fault,
		.native = structure::native_requirement(),
		.fault = {
			.address = address,
			.access = arm64::MemoryAccess::read,
		},
		.compare_fault = true,
		.memory = std::span<const engine::MemoryRegion>{regions},
	});
}

void first_access_fault_metadata(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	const auto address = structure::data_base;
	for (unsigned count = 1; count <= 4U; ++count)
		for (const auto width : {1U, 2U, 4U, 8U}) {
			const auto encoding = representative(count, width);
			for (const auto mode : modes) {
				auto initial = structure::seeded_state(seed++);
				structure::set_base(initial, 2, address);
				char name[136];
				std::snprintf(name, sizeof(name),
					"replicate unmapped count=%u width=%u mode=%u",
					count, width, static_cast<unsigned>(mode));
				harness.check({
					.name = name,
					.instruction = instruction(encoding, mode),
					.initial = initial,
					.expected = initial,
					.event =
						engine::ExpectedEvent::memory_fault,
					.native =
						structure::native_requirement(),
					.fault = {
						.address = address,
						.access =
							arm64::MemoryAccess::read,
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
	std::uint64_t seed = 0xa900'0000'0000'0000ULL;
	cross_page_success(harness, seed);
	cross_page_faults_are_restartable(harness, seed);
	every_boundary_split(harness, seed);
	stack_alignment_precedes_memory(harness, seed);
	every_stack_misalignment(harness, seed);
	permission_fault(harness, seed);
	first_access_fault_metadata(harness, seed);
	harness.print_summary("arm64-simd-structure-replicate-fault");
	return 0;
}
