// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_gcs_test_support.hpp"

#include <array>

namespace gcs = arm64_base_gcs_test;
namespace test = uml::emulator::tests;

namespace {

void all_store_register_fields(test::EngineHarness &harness,
	std::size_t &cases)
{
	for (const auto operation :
	     {gcs::Operation::store, gcs::Operation::store_unprivileged}) {
		for (unsigned base = 0; base < 32; ++base) {
			for (unsigned transfer = 0; transfer < 32; ++transfer) {
				gcs::Page initial_memory;
				gcs::fill_pattern(initial_memory,
					static_cast<unsigned>(operation) * 53U +
					base * 7U + transfer);
				auto expected_memory = initial_memory;
				auto initial = gcs::initial_state();
				auto offset = gcs::page_size / 2 +
					((base * 37U + transfer * 11U) & 0xffU);
				offset = base == 31U ? offset & ~std::size_t{15} :
					offset;
				gcs::require(offset <= gcs::page_size - 8);
				const auto address = gcs::data_base + offset;
				gcs::write_register(initial, base, address, true);
				const auto value =
					gcs::read_register(initial, transfer, false);
				gcs::store_u64(expected_memory, offset, value);
				const auto expected = initial;
				const std::array regions{
					gcs::memory_region(initial_memory, expected_memory),
				};
				const gcs::CaseName name{"%s-b%u-t%u-low%u",
					operation == gcs::Operation::store ?
						"gcsstr" : "gcssttr",
					base, transfer,
					static_cast<unsigned>(address & 15U)};
				harness.check({
					.name = name,
					.instruction =
						gcs::encode(operation, transfer, base),
					.initial = initial,
					.expected = expected,
					.native = gcs::native_gcs,
					.memory = regions,
				});
				++cases;
			}
		}
	}
}

} // namespace

int main()
{
	test::EngineHarness harness;
	std::size_t cases = 0;
	all_store_register_fields(harness, cases);
	gcs::require(cases == 2048);
	gcs::require_counts(harness, cases);
	harness.print_summary("arm64-base-gcs-memory");
}
