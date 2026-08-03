// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_dot_engine_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_fp8_dot_engine_test;

namespace {

template<typename Function>
void for_each_form(Function &&function)
{
	for (const unsigned width : {16U, 32U})
		for (const bool wide : {false, true}) {
			function(test::Form{width, false, wide, 0U});
			const unsigned indices = width == 16U ? 8U : 4U;
			for (unsigned index = 0U; index < indices; ++index)
				function(test::Form{width, true, wide, index});
		}
}

void every_source_pattern_format_mapping_and_direction()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 10'000U;
	for_each_form([&](const test::Form &form) {
		const test::Registers registers{
			28U, 29U,
			form.by_element && form.width == 16U ? 14U : 30U};
		for (unsigned format1 = 0U; format1 < 2U; ++format1)
			for (unsigned format2 = 0U; format2 < 2U; ++format2)
				for (unsigned group = 0U; group < 32U; ++group)
					for (const bool reverse : {false, true}) {
						auto initial = test::state(++seed);
						initial.fpmr = test::fpmr(
							format1, format2,
							(seed >> 2U) &
								(form.width == 16U ?
									15U : 127U),
							(seed & 1U) != 0U);
						for (unsigned byte = 0U; byte < 16U;
						     ++byte) {
							const auto varying =
								static_cast<std::uint8_t>(
									group * 8U +
									(byte & 7U));
							const auto partner =
								static_cast<std::uint8_t>(
									255U - varying);
							initial.vectors[29U].bytes[byte] =
								static_cast<std::byte>(
									reverse ? partner :
										varying);
							initial.vectors[
								registers.second].bytes[byte] =
								static_cast<std::byte>(
									reverse ? varying :
										partner);
						}
						const auto &addends =
							form.width == 16U ?
								test::half_addends :
								test::single_addends;
						for (unsigned lane = 0U;
						     lane < test::lanes(form);
						     ++lane)
							test::write_lane(
								initial.vectors[28U].bytes,
								lane, form.width,
								addends[
									(group + lane) %
									addends.size()]);
						char name[208];
						std::snprintf(name, sizeof(name),
							"fp8-dot-behavior width=%u indexed=%u "
							"wide=%u index=%u f1=%u f2=%u "
							"group=%u reverse=%u",
							form.width,
							static_cast<unsigned>(
								form.by_element),
							static_cast<unsigned>(form.wide),
							form.index, format1, format2,
							group,
							static_cast<unsigned>(reverse));
						test::check(harness, name, form,
							registers, initial);
					}
	});
	harness.print_summary("arm64-fp8-dot-behavior");
}

} // namespace

int main()
{
	every_source_pattern_format_mapping_and_direction();
	return 0;
}
