// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_precision_conversion_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_precision_conversion_test;

namespace {

[[nodiscard]] constexpr std::uint64_t one(unsigned width) noexcept
{
	return width == 16U ? 0x3c00U :
		width == 32U ? 0x3f80'0000U :
			0x3ff0'0000'0000'0000ULL;
}

void every_vector_form_register_edge_and_alias()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed{};
	for (const auto form : test::ordinary_forms) {
		for (const auto registers : test::register_edges) {
			auto initial = test::state(++seed);
			const std::array values{one(form.source_width)};
			test::fill_source(initial, form, registers, values);
			char name[160];
			std::snprintf(name, sizeof(name),
				"floating-precision-allocation operation=%u source=%u "
				"destination=%u upper=%u rd=%u rn=%u",
				static_cast<unsigned>(form.operation),
				form.source_width, form.destination_width,
				static_cast<unsigned>(form.upper),
				registers.destination, registers.source);
			test::check(harness, name, form, registers, initial);
		}
	}
	harness.print_summary("arm64-simd-fcvt-vector-allocation");
}

} // namespace

int main()
{
	every_vector_form_register_edge_and_alias();
	return 0;
}
