// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_ordinary_memory_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>

namespace memory = arm64_simd_ordinary_memory_test;
namespace engine = uml::emulator::tests;
namespace arm64 = uml::cpu::arm64;

namespace {

constexpr std::uint8_t logical_tag = 9;
constexpr std::uint8_t mismatching_tag = 6;

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
				granule - memory::data_base),
			.value = value,
		});
		if (mismatch && *mismatch == index)
			result.mismatch_address = std::max(address, granule);
	}
	return result;
}

enum class SingleForm : unsigned {
	unsigned_offset,
	unscaled,
	post_index,
	pre_index,
	register_offset,
};

constexpr std::array single_forms{
	SingleForm::unsigned_offset,
	SingleForm::unscaled,
	SingleForm::post_index,
	SingleForm::pre_index,
	SingleForm::register_offset,
};

[[nodiscard]] constexpr std::string_view form_name(SingleForm form)
{
	switch (form) {
	case SingleForm::unsigned_offset:
		return "unsigned";
	case SingleForm::unscaled:
		return "unscaled";
	case SingleForm::post_index:
		return "post";
	case SingleForm::pre_index:
		return "pre";
	case SingleForm::register_offset:
		return "register";
	}
	return "unknown";
}

struct SingleAddress final {
	std::uint32_t instruction{};
	std::uint64_t effective{};
	std::uint64_t writeback{};
	bool writes_back{};
	bool stack_tagchecked{};
};

[[nodiscard]] SingleAddress single_address(
	const memory::SingleVariant &variant, SingleForm form,
	std::uint64_t base, unsigned rn, arm64::CpuState &initial)
{
	switch (form) {
	case SingleForm::unsigned_offset:
		return {
			.instruction = memory::unsigned_instruction(
				variant.size_code, variant.operation, 1, rn, 29),
			.effective = base + variant.size,
			.stack_tagchecked = false,
		};
	case SingleForm::unscaled:
		return {
			.instruction = memory::immediate_instruction(
				variant.size_code, variant.operation, 3,
				memory::ImmediateMode::unscaled, rn, 29),
			.effective = base + 3U,
			.stack_tagchecked = false,
		};
	case SingleForm::post_index:
		return {
			.instruction = memory::immediate_instruction(
				variant.size_code, variant.operation, 16,
				memory::ImmediateMode::post_index, rn, 29),
			.effective = base,
			.writeback = base + 16U,
			.writes_back = true,
			.stack_tagchecked = true,
		};
	case SingleForm::pre_index:
		return {
			.instruction = memory::immediate_instruction(
				variant.size_code, variant.operation, 16,
				memory::ImmediateMode::pre_index, rn, 29),
			.effective = base + 16U,
			.writeback = base + 16U,
			.writes_back = true,
			.stack_tagchecked = true,
		};
	case SingleForm::register_offset:
		initial.registers[5] = 8U;
		return {
			.instruction = memory::register_instruction(
				variant.size_code, variant.operation,
				5, 3, false, rn, 29),
			.effective = base + 8U,
			.stack_tagchecked = true,
		};
	}
	return {};
}

void check_single_tag(engine::EngineHarness &harness, std::string_view name,
	const memory::SingleVariant &variant, SingleForm form, unsigned rn,
	std::uint8_t address_tag, bool tco,
	std::span<const engine::AllocationTag> tags,
	std::optional<std::uint64_t> fault_address,
	std::span<const std::byte> page, std::uint64_t seed)
{
	const auto base = memory::data_base + memory::page_size / 2U;
	auto initial = memory::seeded_state(seed);
	initial.set_mte_configuration(true, 0);
	initial.pstate_tco = tco;
	const auto architectural_base =
		rn == 31U ? base : tagged(base, address_tag);
	memory::write_base(initial, rn, architectural_base);
	const auto address =
		single_address(variant, form, architectural_base, rn, initial);
	auto expected = initial;
	const std::array regions{
		engine::MemoryRegion{
			.address = memory::data_base,
			.initial = page,
			.initial_tags = tags,
		},
	};
	if (fault_address) {
		memory::check(harness, name, address.instruction,
			initial, expected, memory::single_native(), regions,
			engine::ExpectedEvent::memory_fault,
			{
				.address = *fault_address,
				.access = variant.load ?
					arm64::MemoryAccess::read :
					arm64::MemoryAccess::write,
			},
			true);
		return;
	}

	auto expected_memory =
		std::vector<std::byte>(page.begin(), page.end());
	const auto untagged_effective =
		address.effective & 0x00ff'ffff'ffff'ffffULL;
	memory::apply_single_oracle(initial, expected, page, expected_memory,
		static_cast<std::size_t>(
			untagged_effective - memory::data_base),
		variant, 29);
	if (address.writes_back)
		memory::write_base(expected, rn, address.writeback);
	const std::array success_regions{
		engine::MemoryRegion{
			.address = memory::data_base,
			.initial = page,
			.expected = std::span<const std::byte>{expected_memory},
			.initial_tags = tags,
		},
	};
	memory::check(harness, name, address.instruction,
		initial, expected, memory::single_native(), success_regions);
}

void gpr_single_tags(engine::EngineHarness &harness,
	std::span<const std::byte> page, std::uint64_t &seed)
{
	const auto base = memory::data_base + memory::page_size / 2U;
	for (const auto &variant : memory::single_variants)
		for (const auto form : single_forms) {
			auto scratch = memory::seeded_state(0);
			const auto address =
				single_address(variant, form, base, 20, scratch);
			const auto physical =
				address.effective & 0x00ff'ffff'ffff'ffffULL;
			const auto matching =
				tags_for(physical, variant.size, logical_tag);
			const auto granules =
				((physical & 15U) + variant.size + 15U) / 16U;
			const auto mismatch = tags_for(
				physical, variant.size, logical_tag, granules - 1U);
			char name[176];
			std::snprintf(name, sizeof(name),
				"%.*s %.*s gpr matching tag",
				static_cast<int>(variant.name.size()),
				variant.name.data(),
				static_cast<int>(form_name(form).size()),
				form_name(form).data());
			check_single_tag(harness, name, variant, form, 20,
				logical_tag, false, matching.values, std::nullopt,
				page, seed++);

			std::snprintf(name, sizeof(name),
				"%.*s %.*s gpr mismatching tag",
				static_cast<int>(variant.name.size()),
				variant.name.data(),
				static_cast<int>(form_name(form).size()),
				form_name(form).data());
			check_single_tag(harness, name, variant, form, 20,
				logical_tag, false, mismatch.values,
				mismatch.mismatch_address, page, seed++);

			std::snprintf(name, sizeof(name),
				"%.*s %.*s gpr tag override",
				static_cast<int>(variant.name.size()),
				variant.name.data(),
				static_cast<int>(form_name(form).size()),
				form_name(form).data());
			check_single_tag(harness, name, variant, form, 20,
				logical_tag, true, mismatch.values, std::nullopt,
				page, seed++);
		}
}

void stack_single_tagchecked_rules(engine::EngineHarness &harness,
	std::span<const std::byte> page, std::uint64_t &seed)
{
	const auto base = memory::data_base + memory::page_size / 2U;
	for (const auto &variant : memory::single_variants)
		for (const auto form : single_forms) {
			auto scratch = memory::seeded_state(0);
			const auto address =
				single_address(variant, form, base, 31, scratch);
			const auto mismatch =
				tags_for(address.effective, variant.size, 0, 0);
			char name[176];
			if (!address.stack_tagchecked) {
				std::snprintf(name, sizeof(name),
					"%.*s %.*s sp ignores allocation tag",
					static_cast<int>(variant.name.size()),
					variant.name.data(),
					static_cast<int>(form_name(form).size()),
					form_name(form).data());
				check_single_tag(harness, name, variant, form, 31,
					0, false, mismatch.values, std::nullopt,
					page, seed++);
				continue;
			}

			std::snprintf(name, sizeof(name),
				"%.*s %.*s sp checks allocation tag",
				static_cast<int>(variant.name.size()),
				variant.name.data(),
				static_cast<int>(form_name(form).size()),
				form_name(form).data());
			check_single_tag(harness, name, variant, form, 31,
				0, false, mismatch.values,
				mismatch.mismatch_address, page, seed++);
			std::snprintf(name, sizeof(name),
				"%.*s %.*s sp tag override",
				static_cast<int>(variant.name.size()),
				variant.name.data(),
				static_cast<int>(form_name(form).size()),
				form_name(form).data());
			check_single_tag(harness, name, variant, form, 31,
				0, true, mismatch.values, std::nullopt,
				page, seed++);
		}
}

[[nodiscard]] constexpr bool pair_writeback(memory::PairMode mode)
{
	return mode == memory::PairMode::post_index ||
		mode == memory::PairMode::pre_index;
}

[[nodiscard]] constexpr bool pair_stack_tagchecked(memory::PairMode mode)
{
	return pair_writeback(mode);
}

void check_pair_tag(engine::EngineHarness &harness, std::string_view name,
	bool load, memory::PairMode mode, unsigned rn,
	std::uint8_t address_tag, bool tco,
	std::span<const engine::AllocationTag> tags,
	std::optional<std::uint64_t> fault_address,
	std::span<const std::byte> page, std::uint64_t seed)
{
	const auto base = memory::data_base + memory::page_size / 2U;
	const auto architectural_base =
		rn == 31U ? base : tagged(base, address_tag);
	constexpr unsigned imm7 = 1;
	const auto adjusted = architectural_base + 16U;
	const auto effective = mode == memory::PairMode::post_index ?
		architectural_base : adjusted;
	const auto physical = effective & 0x00ff'ffff'ffff'ffffULL;
	auto initial = memory::seeded_state(seed);
	initial.set_mte_configuration(true, 0);
	initial.pstate_tco = tco;
	memory::write_base(initial, rn, architectural_base);
	auto expected = initial;
	const std::array regions{
		engine::MemoryRegion{
			.address = memory::data_base,
			.initial = page,
			.initial_tags = tags,
		},
	};
	const auto instruction =
		memory::pair_instruction(load, mode, imm7, rn, 3, 17);
	if (fault_address) {
		memory::check(harness, name, instruction, initial, expected,
			memory::pair_native(), regions,
			engine::ExpectedEvent::memory_fault,
			{
				.address = *fault_address,
				.access = load ?
					arm64::MemoryAccess::read :
					arm64::MemoryAccess::write,
			},
			true);
		return;
	}

	auto expected_memory =
		std::vector<std::byte>(page.begin(), page.end());
	memory::apply_pair_oracle(initial, expected, page, expected_memory,
		static_cast<std::size_t>(physical - memory::data_base),
		load, 3, 17);
	if (pair_writeback(mode))
		memory::write_base(expected, rn, adjusted);
	const std::array success_regions{
		engine::MemoryRegion{
			.address = memory::data_base,
			.initial = page,
			.expected = std::span<const std::byte>{expected_memory},
			.initial_tags = tags,
		},
	};
	memory::check(harness, name, instruction, initial, expected,
		memory::pair_native(), success_regions);
}

void pair_tags(engine::EngineHarness &harness,
	std::span<const std::byte> page, std::uint64_t &seed)
{
	const auto base = memory::data_base + memory::page_size / 2U;
	for (unsigned mode_value = 0; mode_value < 4U; ++mode_value)
		for (const bool load : {false, true}) {
			const auto mode =
				static_cast<memory::PairMode>(mode_value);
			const auto physical =
				mode == memory::PairMode::post_index ?
					base : base + 16U;
			const auto matching =
				tags_for(physical, 32, logical_tag);
			const auto mismatch =
				tags_for(physical, 32, logical_tag, 1);
			char name[160];
			std::snprintf(name, sizeof(name),
				"%s gpr matching tag mode=%u",
				load ? "load-pair" : "store-pair", mode_value);
			check_pair_tag(harness, name, load, mode, 20,
				logical_tag, false, matching.values, std::nullopt,
				page, seed++);
			std::snprintf(name, sizeof(name),
				"%s gpr second-granule mismatch mode=%u",
				load ? "load-pair" : "store-pair", mode_value);
			check_pair_tag(harness, name, load, mode, 20,
				logical_tag, false, mismatch.values,
				mismatch.mismatch_address, page, seed++);
			std::snprintf(name, sizeof(name),
				"%s gpr tag override mode=%u",
				load ? "load-pair" : "store-pair", mode_value);
			check_pair_tag(harness, name, load, mode, 20,
				logical_tag, true, mismatch.values, std::nullopt,
				page, seed++);

			const auto stack_mismatch =
				tags_for(physical, 32, 0, 1);
			if (!pair_stack_tagchecked(mode)) {
				std::snprintf(name, sizeof(name),
					"%s sp ignores allocation tag mode=%u",
					load ? "load-pair" : "store-pair",
					mode_value);
				check_pair_tag(harness, name, load, mode, 31,
					0, false, stack_mismatch.values,
					std::nullopt, page, seed++);
				continue;
			}
			std::snprintf(name, sizeof(name),
				"%s sp checks allocation tag mode=%u",
				load ? "load-pair" : "store-pair", mode_value);
			check_pair_tag(harness, name, load, mode, 31,
				0, false, stack_mismatch.values,
				stack_mismatch.mismatch_address, page, seed++);
			std::snprintf(name, sizeof(name),
				"%s sp tag override mode=%u",
				load ? "load-pair" : "store-pair", mode_value);
			check_pair_tag(harness, name, load, mode, 31,
				0, true, stack_mismatch.values, std::nullopt,
				page, seed++);
		}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	std::vector<std::byte> page(memory::page_size);
	memory::fill_memory(page, 0xe7);
	std::uint64_t seed = 0x14'000;
	gpr_single_tags(harness, page, seed);
	stack_single_tagchecked_rules(harness, page, seed);
	pair_tags(harness, page, seed);
	harness.print_summary("arm64-simd-ordinary-memory-tag");
}
