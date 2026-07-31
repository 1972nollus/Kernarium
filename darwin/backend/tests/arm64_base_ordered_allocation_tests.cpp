// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_ordered_memory_test_support.hpp"

#include <array>
#include <cstdio>

namespace ordered = arm64_ordered_memory_test;
namespace engine = uml::backend::tests;

namespace {

void every_scalar_register_field(engine::EngineHarness &harness)
{
	for (const auto &variant : ordered::scalar_variants)
		for (unsigned rn = 0; rn < 32U; ++rn)
			for (unsigned rt = 0; rt < 32U; ++rt) {
				constexpr int offset = 0;
				auto initial = ordered::seeded_state(
					0x1000'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant.size_code) << 40) |
					(static_cast<std::uint64_t>(variant.operation) << 36) |
					(static_cast<std::uint64_t>(rn) << 5) | rt);
				ordered::set_base(initial, rn,
					ordered::scalar_base_for_target(
						variant, ordered::unmapped_address, offset));
				char name[112];
				ordered::describe(name, sizeof(name), variant.name,
					"allocated", rn, rt);
				harness.check({
					.name = name,
					.instruction = ordered::scalar_instruction(
						variant, offset, rn, rt),
					.initial = initial,
					.expected = initial,
					.event = engine::ExpectedEvent::memory_fault,
					.native = ordered::native(variant),
					.fault = {
						.address = ordered::unmapped_address,
						.access = variant.load ?
							ordered::arm64::MemoryAccess::read :
							ordered::arm64::MemoryAccess::write,
					},
					.compare_fault = true,
				});
			}
}

void reserved_ordered_control_fields(engine::EngineHarness &harness)
{
	for (const auto &variant : ordered::scalar_variants) {
		if (variant.instruction_class != ordered::ScalarClass::ordered)
			continue;
		const auto initial =
			ordered::seeded_state(0x2000'0000'0000'0000ULL);
		const auto allocated =
			ordered::scalar_instruction(variant, 0, 2, 3);
		for (unsigned bad = 0; bad < 31U; ++bad) {
			char status_name[112];
			std::snprintf(status_name, sizeof(status_name),
				"%.*s reserved Rs=%u",
				static_cast<int>(variant.name.size()),
				variant.name.data(), bad);
			harness.check_illegal(status_name,
				(allocated & ~(31U << 16)) | (bad << 16),
				initial, ordered::native(variant));

			char second_name[112];
			std::snprintf(second_name, sizeof(second_name),
				"%.*s reserved Rt2=%u",
				static_cast<int>(variant.name.size()),
				variant.name.data(), bad);
			harness.check_illegal(second_name,
				(allocated & ~(31U << 10)) | (bad << 10),
				initial, ordered::native(variant));
		}
	}
}

void reserved_writeback_widths(engine::EngineHarness &harness)
{
	const auto initial =
		ordered::seeded_state(0x3000'0000'0000'0000ULL);
	for (unsigned size_code = 0; size_code < 2U; ++size_code)
		for (const bool load : {false, true})
			for (unsigned rn = 0; rn < 32U; ++rn)
				for (unsigned rt = 0; rt < 32U; ++rt) {
					const auto instruction = 0x1980'0800U |
						(size_code << 30) |
						(static_cast<unsigned>(load) << 22) |
						(rn << 5) | rt;
					char name[112];
					std::snprintf(name, sizeof(name),
						"reserved ordered writeback size=%u load=%u "
						"rn=%u rt=%u", size_code,
						static_cast<unsigned>(load), rn, rt);
					harness.check_illegal(name, instruction, initial,
						engine::NativeRequirements::features(
							uml::backend::ArmFeature::lrcpc3));
				}
}

void reserved_unscaled_partitions(engine::EngineHarness &harness)
{
	const auto initial =
		ordered::seeded_state(0x4000'0000'0000'0000ULL);
	constexpr std::array reserved{
		std::pair{3U, 2U},
		std::pair{2U, 3U},
		std::pair{3U, 3U},
	};
	for (const auto [size_code, operation] : reserved) {
		for (unsigned imm9 = 0; imm9 < 512U; ++imm9) {
			const auto instruction = 0x1900'0000U |
				(size_code << 30) | (operation << 22) |
				(imm9 << 12) | (2U << 5) | 3U;
			char name[104];
			std::snprintf(name, sizeof(name),
				"reserved ordered unscaled size=%u opc=%u imm9=%u",
				size_code, operation, imm9);
			harness.check_illegal(name, instruction, initial,
				engine::NativeRequirements::features(
					uml::backend::ArmFeature::lrcpc2));
		}
		for (unsigned field = 0; field < 32U; ++field) {
			const auto base = 0x1900'0000U |
				(size_code << 30) | (operation << 22);
			char rn_name[104];
			std::snprintf(rn_name, sizeof(rn_name),
				"reserved ordered unscaled size=%u opc=%u rn=%u",
				size_code, operation, field);
			harness.check_illegal(rn_name,
				base | (field << 5) | 3U, initial,
				engine::NativeRequirements::features(
					uml::backend::ArmFeature::lrcpc2));
			char rt_name[104];
			std::snprintf(rt_name, sizeof(rt_name),
				"reserved ordered unscaled size=%u opc=%u rt=%u",
				size_code, operation, field);
			harness.check_illegal(rt_name,
				base | (2U << 5) | field, initial,
				engine::NativeRequirements::features(
					uml::backend::ArmFeature::lrcpc2));
		}
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	every_scalar_register_field(harness);
	reserved_ordered_control_fields(harness);
	reserved_writeback_widths(harness);
	reserved_unscaled_partitions(harness);
	harness.print_summary("arm64-base-ordered-allocation");
	return 0;
}
