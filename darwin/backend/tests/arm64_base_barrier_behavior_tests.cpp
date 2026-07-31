// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_barrier_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

namespace uml::backend::tests::base_barrier {
namespace {

void check_program(EngineHarness &harness, std::string_view name,
	const std::array<ProgramWord, 4> &code, const arm64::CpuState &initial,
	NativeRequirements native)
{
	auto expected = initial;
	expected.pc = engine_test_code_base + code.size() * sizeof(std::uint32_t);
	harness.check_program({
		.name = name,
		.code = std::span<const ProgramWord>{code},
		.initial = initial,
		.expected = expected,
		.native = native,
	});
}

void standard_sequences(EngineHarness &harness)
{
	for (unsigned option = 0; option < 16U; ++option) {
		const std::array code{
			ProgramWord{0, encode_standard(dmb_base, option)},
			ProgramWord{4, encode_standard(dsb_base, 15U - option)},
			ProgramWord{8, encode_standard(isb_base, option)},
			ProgramWord{12, terminal_system_call},
		};
		char name[96];
		std::snprintf(name, sizeof(name),
			"BASE-0150/0152/0181 ordered sequence option=%u", option);
		check_program(harness, name, code,
			seeded_state(0x4220'0000'0000'0000ULL ^ option),
			NativeRequirements::baseline());
	}
}

void nxs_sequences(EngineHarness &harness)
{
	for (unsigned scope = 0; scope < 4U; ++scope) {
		const std::array code{
			ProgramWord{0, encode_nxs(scope)},
			ProgramWord{4, encode_standard(dmb_base, 3U + scope * 4U)},
			ProgramWord{8, encode_standard(isb_base, 15U)},
			ProgramWord{12, terminal_system_call},
		};
		char name[96];
		std::snprintf(name, sizeof(name),
			"BASE-0152 DSB nXS ordered sequence scope=%u", scope);
		check_program(harness, name, code,
			seeded_state(0x4230'0000'0000'0000ULL ^ scope),
			NativeRequirements::features(ArmFeature::xs));
	}
}

} // namespace
} // namespace uml::backend::tests::base_barrier

int main()
{
	uml::backend::tests::EngineHarness harness;
	uml::backend::tests::base_barrier::standard_sequences(harness);
	uml::backend::tests::base_barrier::nxs_sequences(harness);
	harness.print_summary("arm64-base-barrier-behavior");
	return 0;
}
