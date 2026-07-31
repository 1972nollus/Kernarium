// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_structure_test_support.hpp"

#include <array>
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

template <typename Callback>
void for_each_layout(Callback callback)
{
	for (const auto &form : structure::multiple_forms)
		for (const bool full_vector : {false, true})
			for (unsigned size = 0; size < 4U; ++size)
				if (structure::multiple_arrangement_valid(
					form, full_vector, size))
					callback(form, full_vector, size);
}

void cross_page_success(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(2U * structure::page_size);
	structure::fill_memory(memory, 0x63);
	const auto boundary = structure::data_base + structure::page_size;
	for_each_layout([&](const structure::MultipleForm &form,
			    bool full_vector, unsigned size) {
		const auto transfer =
			structure::multiple_transfer_size(form, full_vector);
		const auto address = boundary - std::max<std::size_t>(1, transfer / 2);
		for (const bool load : {false, true})
			for (const auto mode : modes) {
				char name[152];
				std::snprintf(name, sizeof(name),
					"%.*s cross-page q=%u size=%u load=%u mode=%u",
					static_cast<int>(form.mnemonic.size()),
					form.mnemonic.data(),
					static_cast<unsigned>(full_vector), size,
					static_cast<unsigned>(load),
					static_cast<unsigned>(mode));
				structure::check_multiple_success(
					harness, name, form, full_vector, size, load,
					mode, 2, 30, 5, address, memory, seed++);
			}
	});
}

void cross_page_faults_are_restartable(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x74);
	const auto boundary = structure::data_base + structure::page_size;
	for_each_layout([&](const structure::MultipleForm &form,
			    bool full_vector, unsigned size) {
		const auto transfer =
			structure::multiple_transfer_size(form, full_vector);
		const auto address = boundary - std::max<std::size_t>(1, transfer / 2);
		for (const bool load : {false, true})
			for (const auto mode : modes) {
				auto initial = structure::seeded_state(seed++);
				structure::set_base(initial, 2, address);
				char name[160];
				std::snprintf(name, sizeof(name),
					"%.*s cross-page-fault q=%u size=%u load=%u "
					"mode=%u",
					static_cast<int>(form.mnemonic.size()),
					form.mnemonic.data(),
					static_cast<unsigned>(full_vector), size,
					static_cast<unsigned>(load),
					static_cast<unsigned>(mode));
				structure::check_memory(
					harness, name,
					structure::multiple_instruction(
						full_vector, load,
						structure::post_index(mode),
						structure::encoded_offset_register(mode, 5),
						form.opcode, size, 2, 30),
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
	});
}

void every_boundary_split(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x85);
	const auto boundary = structure::data_base + structure::page_size;
	for (const auto &form : structure::multiple_forms)
		for (const bool full_vector : {false, true}) {
			const auto transfer =
				structure::multiple_transfer_size(form, full_vector);
			for (std::size_t split = 1; split < transfer; ++split)
				for (const bool load : {false, true}) {
					auto initial = structure::seeded_state(seed++);
					structure::set_base(initial, 2, boundary - split);
					char name[152];
					std::snprintf(name, sizeof(name),
						"%.*s every-split q=%u split=%zu load=%u",
						static_cast<int>(form.mnemonic.size()),
						form.mnemonic.data(),
						static_cast<unsigned>(full_vector), split,
						static_cast<unsigned>(load));
					structure::check_memory(
						harness, name,
						structure::multiple_instruction(
							full_vector, load, true, 31,
							form.opcode, 0, 2, 29),
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
	structure::fill_memory(memory, 0x96);
	const auto address =
		structure::data_base + structure::page_size / 2U + 8U;
	for_each_layout([&](const structure::MultipleForm &form,
			    bool full_vector, unsigned size) {
		for (const bool load : {false, true})
			for (const auto mode : modes) {
				auto initial = structure::seeded_state(seed++);
				initial.sp = address;
				char name[152];
				std::snprintf(name, sizeof(name),
					"%.*s misaligned-sp q=%u size=%u load=%u mode=%u",
					static_cast<int>(form.mnemonic.size()),
					form.mnemonic.data(),
					static_cast<unsigned>(full_vector), size,
					static_cast<unsigned>(load),
					static_cast<unsigned>(mode));
				structure::check_memory(
					harness, name,
					structure::multiple_instruction(
						full_vector, load,
						structure::post_index(mode),
						structure::encoded_offset_register(mode, 5),
						form.opcode, size, 31, 30),
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
	});
}

void every_stack_misalignment(engine::EngineHarness &harness,
	std::uint64_t &seed)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x97);
	const auto aligned =
		structure::data_base + structure::page_size / 2U;
	const auto &form = structure::multiple_forms.back();
	for (unsigned misalignment = 1; misalignment < 16U; ++misalignment)
		for (const bool load : {false, true}) {
			const auto address = aligned + misalignment;
			auto initial = structure::seeded_state(seed++);
			initial.sp = address;
			char name[128];
			std::snprintf(name, sizeof(name),
				"%.*s every-sp-misalignment=%u load=%u",
				static_cast<int>(form.mnemonic.size()),
				form.mnemonic.data(), misalignment,
				static_cast<unsigned>(load));
			structure::check_memory(
				harness, name,
				structure::multiple_instruction(
					true, load, false, 0, form.opcode, 0, 31,
					30),
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
	structure::fill_memory(memory, 0x98);
	const auto address =
		structure::data_base + structure::page_size / 2U;
	const auto &form = structure::multiple_forms.back();
	for (const bool load : {false, true}) {
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
				"multiple read-permission fault" :
				"multiple write-permission fault",
			.instruction = structure::multiple_instruction(
				true, load, false, 0, form.opcode, 0, 2, 30),
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
	for (const auto &form : structure::multiple_forms)
		for (const bool load : {false, true})
			for (const auto mode : modes) {
				auto initial = structure::seeded_state(seed++);
				structure::set_base(initial, 2, address);
				char name[136];
				std::snprintf(name, sizeof(name),
					"%.*s first-page-unmapped load=%u mode=%u",
					static_cast<int>(form.mnemonic.size()),
					form.mnemonic.data(),
					static_cast<unsigned>(load),
					static_cast<unsigned>(mode));
				harness.check({
					.name = name,
					.instruction = structure::multiple_instruction(
						true, load, structure::post_index(mode),
						structure::encoded_offset_register(mode, 5),
						form.opcode, 0, 2, 30),
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
				});
			}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	std::uint64_t seed = 0xa300'0000'0000'0000ULL;
	cross_page_success(harness, seed);
	cross_page_faults_are_restartable(harness, seed);
	every_boundary_split(harness, seed);
	stack_alignment_precedes_memory(harness, seed);
	every_stack_misalignment(harness, seed);
	permission_faults(harness, seed);
	first_access_fault_metadata(harness, seed);
	harness.print_summary("arm64-simd-structure-multiple-fault");
	return 0;
}
