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

void every_scale_saturation_and_rounding_boundary()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 30'000U;
	for_each_form([&](const test::Form &form) {
		const test::Registers registers{
			28U, 29U,
			form.by_element && form.width == 16U ? 14U : 30U};
		const unsigned scales = form.width == 16U ? 16U : 128U;
		for (unsigned format1 = 0U; format1 < 2U; ++format1)
			for (unsigned format2 = 0U; format2 < 2U; ++format2)
				for (unsigned scale = 0U; scale < scales; ++scale)
					for (unsigned sample = 0U; sample < 4U;
					     ++sample) {
						auto initial = test::state(++seed);
						initial.fpmr = test::fpmr(
							format1, format2, scale,
							(sample & 1U) != 0U);
						for (unsigned byte = 0U; byte < 16U;
						     ++byte) {
							initial.vectors[29U].bytes[byte] =
								static_cast<std::byte>(
									test::representative_inputs[
										(sample * 5U + byte) %
										test::representative_inputs.size()]);
							initial.vectors[
								registers.second].bytes[byte] =
								static_cast<std::byte>(
									test::representative_inputs[
										(sample * 7U +
										 byte * 3U + 1U) %
										test::representative_inputs.size()]);
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
									(scale + lane +
									 sample * 3U) %
									addends.size()]);
						char name[208];
						std::snprintf(name, sizeof(name),
							"fp8-dot-scale width=%u indexed=%u "
							"wide=%u index=%u f1=%u f2=%u "
							"scale=%u sample=%u",
							form.width,
							static_cast<unsigned>(
								form.by_element),
							static_cast<unsigned>(form.wide),
							form.index, format1, format2,
							scale, sample);
						test::check(harness, name, form,
							registers, initial);
					}
	});
	harness.print_summary("arm64-fp8-dot-scale");
}

} // namespace

int main()
{
	every_scale_saturation_and_rounding_boundary();
	return 0;
}
