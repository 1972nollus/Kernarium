// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_immediate_memory_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = uml::backend::tests::base_immediate_memory;
namespace engine_tests = uml::backend::tests;
namespace arm64 = uml::backend::arm64;

namespace {

void unmapped_faults(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < test::variants.size(); ++variant_index) {
		const auto &variant = test::variants[variant_index];
		for (const auto form : test::all_forms) {
			const auto target = test::data_base +
				12U * test::page_size +
				variant_index * 64U +
				static_cast<unsigned>(form) * 8U;
			const auto immediate =
				form == test::Form::unsigned_offset ? 1U :
				test::encode_imm9(-7);
			auto initial = test::seeded_state(
				0xa000'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(
					variant_index) << 8) |
				static_cast<unsigned>(form));
			initial.registers[2] = test::base_for_address(
				variant, form, target, immediate);
			initial.registers[3] = test::store_value;
			char name[112];
			std::snprintf(name, sizeof(name),
				"%.*s %.*s unmapped fault",
				static_cast<int>(variant.name.size()),
				variant.name.data(),
				static_cast<int>(test::form_name(form).size()),
				test::form_name(form).data());
			harness.check({
				.name = name,
				.instruction = test::encode(
					variant, form, immediate, 2, 3),
				.initial = initial,
				.expected = initial,
				.event = engine_tests::ExpectedEvent::memory_fault,
				.native =
					engine_tests::NativeRequirements::baseline(),
				.fault = {
					.address = target,
					.access = variant.load ?
						arm64::MemoryAccess::read :
						arm64::MemoryAccess::write,
				},
				.compare_fault = true,
			});
		}
	}
}

void discarded_load_still_faults(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < test::variants.size(); ++variant_index) {
		const auto &variant = test::variants[variant_index];
		if (!variant.load)
			continue;
		for (const auto form : test::all_forms) {
			const auto target = test::data_base +
				16U * test::page_size +
				variant_index * 64U +
				static_cast<unsigned>(form) * 8U;
			const unsigned immediate = 0;
			auto initial = test::seeded_state(
				0xa100'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(
					variant_index) << 8) |
				static_cast<unsigned>(form));
			initial.registers[2] = target;
			char name[112];
			std::snprintf(name, sizeof(name),
				"%.*s %.*s XZR fault",
				static_cast<int>(variant.name.size()),
				variant.name.data(),
				static_cast<int>(test::form_name(form).size()),
				test::form_name(form).data());
			harness.check({
				.name = name,
				.instruction = test::encode(
					variant, form, immediate, 2, 31),
				.initial = initial,
				.expected = initial,
				.event = engine_tests::ExpectedEvent::memory_fault,
				.native =
					engine_tests::NativeRequirements::baseline(),
				.fault = {
					.address = target,
					.access = arm64::MemoryAccess::read,
				},
				.compare_fault = true,
			});
		}
	}
}

void second_page_fault_atomicity(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < test::variants.size(); ++variant_index) {
		const auto &variant = test::variants[variant_index];
		if (variant.size == 1U)
			continue;
		for (const auto form : test::all_forms) {
			std::vector<std::byte> memory(test::page_size);
			test::fill_memory(memory, variant_index * 61U +
				static_cast<unsigned>(form));
			const auto target = test::data_base +
				test::page_size - variant.size / 2U;
			auto initial = test::seeded_state(
				0xa200'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(
					variant_index) << 8) |
				static_cast<unsigned>(form));
			initial.registers[2] = target;
			initial.registers[3] = test::store_value;
			const std::array regions{
				engine_tests::MemoryRegion{
					.address = test::data_base,
					.initial = memory,
					.expected = memory,
				},
			};
			char name[120];
			std::snprintf(name, sizeof(name),
				"%.*s %.*s second-page fault",
				static_cast<int>(variant.name.size()),
				variant.name.data(),
				static_cast<int>(test::form_name(form).size()),
				test::form_name(form).data());
			harness.check({
				.name = name,
				.instruction =
					test::encode(variant, form, 0, 2, 3),
				.initial = initial,
				.expected = initial,
				.event = engine_tests::ExpectedEvent::memory_fault,
				.native =
					engine_tests::NativeRequirements::baseline(),
				.fault = {
					.address = test::data_base +
						test::page_size,
					.access = variant.load ?
						arm64::MemoryAccess::read :
						arm64::MemoryAccess::write,
				},
				.compare_fault = true,
				.memory =
					std::span<const engine_tests::MemoryRegion>{
						regions},
			});
		}
	}
}

void stack_alignment_precedes_memory(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < test::variants.size(); ++variant_index) {
		const auto &variant = test::variants[variant_index];
		for (const auto form : test::all_forms) {
			std::vector<std::byte> memory(test::page_size);
			test::fill_memory(memory, variant_index * 67U +
				static_cast<unsigned>(form));
			auto initial = test::seeded_state(
				0xa300'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(
					variant_index) << 8) |
				static_cast<unsigned>(form));
			initial.sp = test::data_base +
				test::page_size / 2U + 1U;
			initial.registers[3] = test::store_value;
			/* For adjusted forms, -1 would make the memory address aligned
			 * and valid. The architectural SP fault must still win.
			 */
			const auto immediate =
				form == test::Form::unsigned_offset ||
					form == test::Form::post_index ?
				0U : test::encode_imm9(-1);
			const std::array regions{
				engine_tests::MemoryRegion{
					.address = test::data_base,
					.initial = memory,
					.expected = memory,
				},
			};
			char name[120];
			std::snprintf(name, sizeof(name),
				"%.*s %.*s SP alignment",
				static_cast<int>(variant.name.size()),
				variant.name.data(),
				static_cast<int>(test::form_name(form).size()),
				test::form_name(form).data());
			harness.check({
				.name = name,
				.instruction = test::encode(
					variant, form, immediate, 31, 3),
				.initial = initial,
				.expected = initial,
				.event = engine_tests::ExpectedEvent::memory_fault,
				.native =
					engine_tests::NativeRequirements::baseline(),
				.fault = {
					.address = initial.sp,
					.access = variant.load ?
						arm64::MemoryAccess::read :
						arm64::MemoryAccess::write,
				},
				.compare_fault = true,
				.memory =
					std::span<const engine_tests::MemoryRegion>{
						regions},
			});
		}
	}
}

void permission_faults(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < test::variants.size(); ++variant_index) {
		const auto &variant = test::variants[variant_index];
		std::vector<std::byte> memory(test::page_size);
		test::fill_memory(memory, variant_index * 71U);
		auto initial = test::seeded_state(
			0xa400'0000'0000'0000ULL | variant_index);
		initial.registers[2] = test::data_base + 128U;
		initial.registers[3] = test::store_value;
		const std::array regions{
			engine_tests::MemoryRegion{
				.address = test::data_base,
				.initial = memory,
				.expected = memory,
				.readable = variant.load ? false : true,
				.writable = false,
			},
		};
		char name[96];
		std::snprintf(name, sizeof(name), "%.*s permission fault",
			static_cast<int>(variant.name.size()), variant.name.data());
		harness.check({
			.name = name,
			.instruction = test::encode(
				variant, test::Form::unscaled, 0, 2, 3),
			.initial = initial,
			.expected = initial,
			.event = engine_tests::ExpectedEvent::memory_fault,
			.native = engine_tests::NativeRequirements::baseline(),
			.fault = {
				.address = initial.registers[2],
				.access = variant.load ?
					arm64::MemoryAccess::read :
					arm64::MemoryAccess::write,
			},
			.compare_fault = true,
			.memory = std::span<const engine_tests::MemoryRegion>{
				regions},
		});
	}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	unmapped_faults(harness);
	discarded_load_still_faults(harness);
	second_page_fault_atomicity(harness);
	stack_alignment_precedes_memory(harness);
	permission_faults(harness);
	test::require_engine_counts(harness, 233U);
	harness.print_summary("arm64-base-immediate-memory-fault");
	return 0;
}
