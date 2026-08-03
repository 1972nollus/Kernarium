// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_structure_test_support.hpp"

#include <array>
#include <cstdlib>
#include <cstdio>
#include <optional>

namespace structure = arm64_simd_structure_test;
namespace engine = uml::emulator::tests;
namespace arm64 = uml::cpu::arm64;

namespace {

constexpr std::uint8_t logical_tag = 9;
constexpr std::uint8_t mismatching_tag = 6;
constexpr std::array modes{
	structure::AddressMode::no_offset,
	structure::AddressMode::post_immediate,
	structure::AddressMode::post_register,
};

[[nodiscard]] constexpr std::uint64_t tagged(
	std::uint64_t address, std::uint8_t tag)
{
	return (address & 0x00ff'ffff'ffff'ffffULL) |
		(static_cast<std::uint64_t>(tag & 15U) << 56);
}

struct Tags final {
	std::vector<engine::AllocationTag> values;
	std::uint64_t mismatch_address{};
};

[[nodiscard]] Tags tags_for(std::uint64_t address, std::size_t transfer_size,
	std::uint8_t match, std::optional<std::size_t> mismatch = std::nullopt)
{
	Tags result;
	const auto first_granule = address & ~std::uint64_t{15};
	const auto end = address + transfer_size;
	std::size_t index = 0;
	for (auto granule = first_granule; granule < end;
	     granule += 16U, ++index) {
		const auto value = mismatch && *mismatch == index ?
			mismatching_tag : match;
		result.values.push_back({
			.offset = static_cast<std::size_t>(
				granule - structure::data_base),
			.value = value,
		});
		if (mismatch && *mismatch == index)
			result.mismatch_address = std::max(address, granule);
	}
	return result;
}

[[nodiscard]] std::size_t mismatch_granule(std::size_t transfer_size)
{
	return transfer_size > 16U ? 1U : 0U;
}

[[nodiscard]] structure::SingleForm lane_form(
	unsigned count, std::size_t width, bool load, unsigned &extra,
	unsigned &opcode, unsigned &scale_bit, unsigned &size)
{
	extra = (count - 1U) & 1U;
	const unsigned scale = width == 1U ? 0U : width == 2U ? 1U : 2U;
	opcode = (scale << 1) | static_cast<unsigned>(count >= 3U);
	scale_bit = 0;
	size = width == 8U ? 1U : 0U;
	const auto form =
		structure::decode_single(true, load, extra, opcode, scale_bit, size);
	if (!form.valid || form.register_count != count ||
	    form.element_size != width)
		std::abort();
	return form;
}

[[nodiscard]] structure::SingleForm replicate_form(
	unsigned count, std::size_t width, unsigned &extra, unsigned &opcode,
	unsigned &size)
{
	extra = (count - 1U) & 1U;
	opcode = count <= 2U ? 6U : 7U;
	size = width == 1U ? 0U : width == 2U ? 1U : width == 4U ? 2U : 3U;
	const auto form =
		structure::decode_single(true, true, extra, opcode, 0, size);
	if (!form.valid || form.register_count != count ||
	    form.element_size != width)
		std::abort();
	return form;
}

void check_multiple(engine::EngineHarness &harness, std::string_view name,
	const structure::MultipleForm &form, bool load,
	structure::AddressMode mode, unsigned rn, std::uint8_t address_tag,
	bool tco, std::span<const engine::AllocationTag> tags,
	std::optional<std::uint64_t> fault_address,
	std::span<const std::byte> memory, std::uint64_t seed,
	bool full_vector = true, unsigned size = 0)
{
	const auto address =
		structure::data_base + structure::page_size / 2U;
	const unsigned rm = 5;
	auto initial = structure::seeded_state(seed);
	initial.set_mte_configuration(true, 0);
	initial.pstate_tco = tco;
	structure::set_base(initial, rn,
		rn == 31U ? address : tagged(address, address_tag));
	auto expected = initial;
	const auto instruction = structure::multiple_instruction(
		full_vector, load, structure::post_index(mode),
		structure::encoded_offset_register(mode, rm), form.opcode, size,
		rn, 29);
	if (fault_address) {
		structure::check_memory(harness, name, instruction, initial, expected,
			memory, {}, engine::ExpectedEvent::memory_fault,
			{
				.address = *fault_address,
				.access = load ?
					arm64::MemoryAccess::read :
					arm64::MemoryAccess::write,
			},
			true, tags);
		return;
	}

	auto expected_memory =
		std::vector<std::byte>(memory.begin(), memory.end());
	structure::apply_multiple_oracle(initial, expected, memory,
		expected_memory,
		static_cast<std::size_t>(address - structure::data_base),
		form, full_vector, size, load, 29);
	structure::apply_writeback(initial, expected, mode, rn, rm,
		structure::multiple_transfer_size(form, full_vector));
	structure::check_memory(harness, name, instruction, initial, expected,
		memory, expected_memory, engine::ExpectedEvent::system_call, {},
		false, tags);
}

void check_single(engine::EngineHarness &harness, std::string_view name,
	const structure::SingleForm &form, bool load, unsigned extra,
	unsigned opcode, unsigned scale_bit, unsigned size,
	structure::AddressMode mode, unsigned rn, std::uint8_t address_tag,
	bool tco, std::span<const engine::AllocationTag> tags,
	std::optional<std::uint64_t> fault_address,
	std::span<const std::byte> memory, std::uint64_t seed,
	std::uint64_t address_override = 0, bool full_vector = true)
{
	const auto address = address_override == 0 ?
		structure::data_base + structure::page_size / 2U :
		address_override;
	const unsigned rm = 5;
	auto initial = structure::seeded_state(seed);
	initial.set_mte_configuration(true, 0);
	initial.pstate_tco = tco;
	structure::set_base(initial, rn,
		rn == 31U ? address : tagged(address, address_tag));
	auto expected = initial;
	const auto instruction = structure::single_instruction(
		full_vector, load, structure::post_index(mode), extra,
		structure::encoded_offset_register(mode, rm), opcode, scale_bit,
		size, rn, 29);
	if (fault_address) {
		structure::check_memory(harness, name, instruction, initial, expected,
			memory, {}, engine::ExpectedEvent::memory_fault,
			{
				.address = *fault_address,
				.access = load ?
					arm64::MemoryAccess::read :
					arm64::MemoryAccess::write,
			},
			true, tags);
		return;
	}

	auto expected_memory =
		std::vector<std::byte>(memory.begin(), memory.end());
	structure::apply_single_oracle(initial, expected, memory, expected_memory,
		static_cast<std::size_t>(address - structure::data_base),
		form, full_vector, load, 29);
	structure::apply_writeback(initial, expected, mode, rn, rm,
		form.register_count * form.element_size);
	structure::check_memory(harness, name, instruction, initial, expected,
		memory, expected_memory, engine::ExpectedEvent::system_call, {},
		false, tags);
}

void matching_and_mismatching_gpr_tags(engine::EngineHarness &harness,
	std::span<const std::byte> memory, std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U;
	for (const auto &form : structure::multiple_forms)
		for (const bool full_vector : {false, true})
			for (unsigned size = 0; size < 4U; ++size) {
				if (!structure::multiple_arrangement_valid(
					form, full_vector, size))
					continue;
				for (const bool load : {false, true})
					for (const auto mode : modes) {
						const auto transfer =
							structure::multiple_transfer_size(
								form, full_vector);
						const auto matching = tags_for(
							address, transfer, logical_tag);
						const auto mismatch = tags_for(
							address, transfer, logical_tag,
							mismatch_granule(transfer));
						char name[176];
						std::snprintf(name, sizeof(name),
							"%.*s matching-tag q=%u size=%u "
							"load=%u mode=%u",
							static_cast<int>(
								form.mnemonic.size()),
							form.mnemonic.data(),
							static_cast<unsigned>(
								full_vector),
							size,
							static_cast<unsigned>(load),
							static_cast<unsigned>(mode));
						check_multiple(
							harness, name, form, load, mode,
							2, logical_tag, false,
							matching.values, std::nullopt,
							memory, seed++, full_vector,
							size);
						std::snprintf(name, sizeof(name),
							"%.*s mismatching-tag q=%u size=%u "
							"load=%u mode=%u",
							static_cast<int>(
								form.mnemonic.size()),
							form.mnemonic.data(),
							static_cast<unsigned>(
								full_vector),
							size,
							static_cast<unsigned>(load),
							static_cast<unsigned>(mode));
						check_multiple(
							harness, name, form, load, mode,
							2, logical_tag, false,
							mismatch.values,
							mismatch.mismatch_address, memory,
							seed++, full_vector, size);
					}
			}

	for (const bool full_vector : {false, true})
		for (const bool load : {false, true})
			for (unsigned extra = 0; extra < 2U; ++extra)
				for (unsigned opcode = 0; opcode < 6U; ++opcode)
					for (unsigned scale_bit = 0; scale_bit < 2U;
					     ++scale_bit)
						for (unsigned size = 0; size < 4U;
						     ++size) {
							const auto form =
								structure::decode_single(
									full_vector, load,
									extra, opcode,
									scale_bit, size);
							if (!form.valid)
								continue;
				const auto transfer =
					form.register_count * form.element_size;
				const auto matching =
					tags_for(address, transfer, logical_tag);
				const auto mismatch = tags_for(address, transfer,
					logical_tag, mismatch_granule(transfer));
				for (const auto mode : modes) {
					char name[184];
					std::snprintf(name, sizeof(name),
						"lane matching-tag q=%u load=%u r=%u "
						"opcode=%u s=%u size=%u mode=%u",
						static_cast<unsigned>(full_vector),
						static_cast<unsigned>(load),
						extra, opcode, scale_bit, size,
						static_cast<unsigned>(mode));
					check_single(harness, name, form, load, extra,
						opcode, scale_bit, size, mode, 2,
						logical_tag, false, matching.values,
						std::nullopt, memory, seed++, 0,
						full_vector);
					std::snprintf(name, sizeof(name),
						"lane mismatching-tag q=%u load=%u r=%u "
						"opcode=%u s=%u size=%u mode=%u",
						static_cast<unsigned>(full_vector),
						static_cast<unsigned>(load),
						extra, opcode, scale_bit, size,
						static_cast<unsigned>(mode));
					check_single(harness, name, form, load, extra,
						opcode, scale_bit, size, mode, 2,
						logical_tag, false, mismatch.values,
						mismatch.mismatch_address, memory,
						seed++, 0, full_vector);
				}
			}

	for (const bool full_vector : {false, true})
		for (unsigned extra = 0; extra < 2U; ++extra)
			for (unsigned opcode = 6; opcode < 8U; ++opcode)
				for (unsigned size = 0; size < 4U; ++size) {
					const auto form = structure::decode_single(
						full_vector, true, extra, opcode, 0,
						size);
					if (!form.valid)
						std::abort();
					const auto transfer =
						form.register_count * form.element_size;
					const auto matching =
						tags_for(address, transfer, logical_tag);
					const auto mismatch = tags_for(
						address, transfer, logical_tag,
						mismatch_granule(transfer));
					for (const auto mode : modes) {
						char name[168];
						std::snprintf(name, sizeof(name),
							"replicate matching-tag q=%u r=%u "
							"opcode=%u size=%u mode=%u",
							static_cast<unsigned>(
								full_vector),
							extra, opcode, size,
							static_cast<unsigned>(mode));
						check_single(
							harness, name, form, true, extra,
							opcode, 0, size, mode, 2,
							logical_tag, false,
							matching.values, std::nullopt,
							memory, seed++, 0,
							full_vector);
						std::snprintf(name, sizeof(name),
							"replicate mismatching-tag q=%u "
							"r=%u opcode=%u size=%u mode=%u",
							static_cast<unsigned>(
								full_vector),
							extra, opcode, size,
							static_cast<unsigned>(mode));
						check_single(
							harness, name, form, true, extra,
							opcode, 0, size, mode, 2,
							logical_tag, false,
							mismatch.values,
							mismatch.mismatch_address, memory,
							seed++, 0, full_vector);
					}
				}
}

void unaligned_transfer_checks_every_touched_granule(
	engine::EngineHarness &harness, std::span<const std::byte> memory,
	std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U + 12U;
	for (const bool load : {false, true}) {
		unsigned extra;
		unsigned opcode;
		unsigned scale_bit;
		unsigned size;
		const auto form =
			lane_form(1, 8, load, extra, opcode, scale_bit, size);
		const auto matching = tags_for(address, 8, logical_tag);
		const auto mismatch = tags_for(address, 8, logical_tag, 1);
		char name[120];
		std::snprintf(name, sizeof(name),
			"lane unaligned matching two-granule load=%u",
			static_cast<unsigned>(load));
		check_single(harness, name, form, load, extra, opcode,
			scale_bit, size, structure::AddressMode::no_offset, 2,
			logical_tag, false, matching.values, std::nullopt, memory,
			seed++, address);
		std::snprintf(name, sizeof(name),
			"lane unaligned second-granule mismatch load=%u",
			static_cast<unsigned>(load));
		check_single(harness, name, form, load, extra, opcode,
			scale_bit, size, structure::AddressMode::no_offset, 2,
			logical_tag, false, mismatch.values,
			mismatch.mismatch_address, memory, seed++, address);
	}
}

void mixed_permission_and_tag_fault_priority(
	engine::EngineHarness &harness, std::uint64_t &seed)
{
	std::vector<std::byte> first(structure::page_size);
	std::vector<std::byte> second(structure::page_size);
	structure::fill_memory(first, 0x874);
	structure::fill_memory(second, 0x985);
	const auto boundary = structure::data_base + structure::page_size;
	const auto address = boundary - 16U;
	const auto &form = structure::multiple_forms.back();
	const auto instruction = structure::multiple_instruction(
		true, false, false, 0, form.opcode, 0, 2, 29);
	const std::array first_mismatch{
		engine::AllocationTag{
			.offset = structure::page_size - 16U,
			.value = mismatching_tag,
		},
	};
	const std::array second_mismatch{
		engine::AllocationTag{.offset = 0, .value = mismatching_tag},
	};

	{
		auto initial = structure::seeded_state(seed++);
		initial.set_mte_configuration(true, 0);
		structure::set_base(initial, 2, tagged(address, 0));
		const std::array regions{
			engine::MemoryRegion{
				.address = structure::data_base,
				.initial = first,
				.readable = true,
				.writable = false,
			},
			engine::MemoryRegion{
				.address = boundary,
				.initial = second,
				.initial_tags = second_mismatch,
			},
		};
		harness.check({
			.name = "structure permission fault precedes later tag fault",
			.instruction = instruction,
			.initial = initial,
			.expected = initial,
			.event = engine::ExpectedEvent::memory_fault,
			.native = structure::native_requirement(),
			.fault = {
				.address = address,
				.access = arm64::MemoryAccess::write,
			},
			.compare_fault = true,
			.memory = std::span<const engine::MemoryRegion>{regions},
		});
	}

	{
		auto initial = structure::seeded_state(seed++);
		initial.set_mte_configuration(true, 0);
		structure::set_base(initial, 2, tagged(address, 0));
		const std::array regions{
			engine::MemoryRegion{
				.address = structure::data_base,
				.initial = first,
				.initial_tags = first_mismatch,
			},
			engine::MemoryRegion{
				.address = boundary,
				.initial = second,
				.readable = true,
				.writable = false,
			},
		};
		harness.check({
			.name = "structure tag fault precedes later permission fault",
			.instruction = instruction,
			.initial = initial,
			.expected = initial,
			.event = engine::ExpectedEvent::memory_fault,
			.native = structure::native_requirement(),
			.fault = {
				.address = address,
				.access = arm64::MemoryAccess::write,
			},
			.compare_fault = true,
			.memory = std::span<const engine::MemoryRegion>{regions},
		});
	}

	{
		const auto split_element_address = boundary - 4U;
		const auto &contiguous = structure::multiple_forms.front();
		const auto split_element_instruction =
			structure::multiple_instruction(
				true, false, false, 0, contiguous.opcode, 3, 2, 29);
		auto initial = structure::seeded_state(seed++);
		initial.set_mte_configuration(true, 0);
		structure::set_base(
			initial, 2, tagged(split_element_address, 0));
		const std::array regions{
			engine::MemoryRegion{
				.address = structure::data_base,
				.initial = first,
				.initial_tags = first_mismatch,
			},
			engine::MemoryRegion{
				.address = boundary,
				.initial = second,
				.readable = true,
				.writable = false,
			},
		};
		harness.check({
			.name =
				"contiguous d element permission fault precedes its tag fault",
			.instruction = split_element_instruction,
			.initial = initial,
			.expected = initial,
			.event = engine::ExpectedEvent::memory_fault,
			.native = structure::native_requirement(),
			.fault = {
				.address = boundary,
				.access = arm64::MemoryAccess::write,
			},
			.compare_fault = true,
			.memory = std::span<const engine::MemoryRegion>{regions},
		});
	}
}

void stack_tagchecked_and_override_rules(engine::EngineHarness &harness,
	std::span<const std::byte> memory, std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U;
	const auto nonzero = tags_for(address, 64, mismatching_tag);
	const auto zero = tags_for(address, 64, 0);
	const auto &multiple = structure::multiple_forms.back();
	for (const bool load : {false, true}) {
		char name[112];
		std::snprintf(name, sizeof(name),
			"multiple sp-no-offset ignores tags load=%u",
			static_cast<unsigned>(load));
		check_multiple(harness, name, multiple, load,
			structure::AddressMode::no_offset, 31, 0, false,
			nonzero.values, std::nullopt, memory, seed++);
		std::snprintf(name, sizeof(name),
			"multiple sp-post checks tags load=%u",
			static_cast<unsigned>(load));
		check_multiple(harness, name, multiple, load,
			structure::AddressMode::post_immediate, 31, 0, false,
			nonzero.values, address, memory, seed++);
		std::snprintf(name, sizeof(name),
			"multiple sp-post matching-zero load=%u",
			static_cast<unsigned>(load));
		check_multiple(harness, name, multiple, load,
			structure::AddressMode::post_register, 31, 0, false,
			zero.values, std::nullopt, memory, seed++);
		std::snprintf(name, sizeof(name),
			"multiple override bypass load=%u",
			static_cast<unsigned>(load));
		check_multiple(harness, name, multiple, load,
			structure::AddressMode::post_immediate, 2, logical_tag,
			true, nonzero.values, std::nullopt, memory, seed++);
	}

	for (const bool load : {false, true}) {
		unsigned extra;
		unsigned opcode;
		unsigned scale_bit;
		unsigned size;
		const auto form =
			lane_form(4, 8, load, extra, opcode, scale_bit, size);
		char name[112];
		std::snprintf(name, sizeof(name),
			"lane sp-no-offset ignores tags load=%u",
			static_cast<unsigned>(load));
		check_single(harness, name, form, load, extra, opcode,
			scale_bit, size, structure::AddressMode::no_offset, 31,
			0, false, nonzero.values, std::nullopt, memory, seed++);
		std::snprintf(name, sizeof(name),
			"lane sp-post checks tags load=%u",
			static_cast<unsigned>(load));
		check_single(harness, name, form, load, extra, opcode,
			scale_bit, size, structure::AddressMode::post_immediate,
			31, 0, false, nonzero.values, address, memory, seed++);
		std::snprintf(name, sizeof(name),
			"lane sp-post matching-zero load=%u",
			static_cast<unsigned>(load));
		check_single(harness, name, form, load, extra, opcode,
			scale_bit, size, structure::AddressMode::post_register,
			31, 0, false, zero.values, std::nullopt, memory, seed++);
		std::snprintf(name, sizeof(name),
			"lane override bypass load=%u",
			static_cast<unsigned>(load));
		check_single(harness, name, form, load, extra, opcode,
			scale_bit, size, structure::AddressMode::post_immediate,
			2, logical_tag, true, nonzero.values, std::nullopt,
			memory, seed++);
	}

	unsigned extra;
	unsigned opcode;
	unsigned size;
	const auto form = replicate_form(4, 8, extra, opcode, size);
	check_single(harness, "replicate sp-no-offset ignores tags", form, true,
		extra, opcode, 0, size, structure::AddressMode::no_offset, 31,
		0, false, nonzero.values, std::nullopt, memory, seed++);
	check_single(harness, "replicate sp-post checks tags", form, true,
		extra, opcode, 0, size, structure::AddressMode::post_immediate,
		31, 0, false, nonzero.values, address, memory, seed++);
	check_single(harness, "replicate sp-post matching-zero", form, true,
		extra, opcode, 0, size, structure::AddressMode::post_register,
		31, 0, false, zero.values, std::nullopt, memory, seed++);
	check_single(harness, "replicate override bypass", form, true, extra,
		opcode, 0, size, structure::AddressMode::post_immediate, 2,
		logical_tag, true, nonzero.values, std::nullopt, memory, seed++);
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x763);
	std::uint64_t seed = 0xab00'0000'0000'0000ULL;
	matching_and_mismatching_gpr_tags(harness, memory, seed);
	unaligned_transfer_checks_every_touched_granule(
		harness, memory, seed);
	mixed_permission_and_tag_fault_priority(harness, seed);
	stack_tagchecked_and_override_rules(harness, memory, seed);
	harness.print_summary("arm64-simd-structure-tag");
	return 0;
}
