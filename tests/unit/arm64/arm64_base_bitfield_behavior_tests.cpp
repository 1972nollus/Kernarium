// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_bitfield_test_support.hpp"

#include <array>

namespace test = arm64_base_bitfield_test;

namespace {

void exhaustive_masks_and_values(test::engine_tests::EngineHarness &harness)
{
	const std::array values{
		std::array{std::uint64_t{0}, std::uint64_t{0}},
		std::array{~std::uint64_t{0}, std::uint64_t{0}},
		std::array{std::uint64_t{0}, ~std::uint64_t{0}},
		std::array{std::uint64_t{0x0123'4567'89ab'cdefULL},
			std::uint64_t{0xfedc'ba98'7654'3210ULL}},
		std::array{std::uint64_t{0x8000'0000'0000'0001ULL},
			std::uint64_t{0xaaaa'5555'aaaa'5555ULL}},
	};
	unsigned seed = 60'000U;
	for (const bool wide : {false, true}) {
		const unsigned width = wide ? 64U : 32U;
		for (unsigned operation = 0; operation < 3U; ++operation)
			for (unsigned immr = 0; immr < width; ++immr)
				for (unsigned imms = 0; imms < width; ++imms)
					for (const auto &value : values) {
						auto initial = test::state(++seed);
						initial.registers[1] = value[0];
						initial.registers[7] = value[1];
						constexpr std::array names{
							"BASE-0360 SBFM", "BASE-0039 BFM",
							"BASE-0495 UBFM",
						};
						test::check(harness,
							names[operation], wide,
							operation, immr, imms, 1U,
							7U, initial);
					}
	}
}

void insertion_alias_ranges(test::engine_tests::EngineHarness &harness,
	bool wide, unsigned operation, std::string_view name,
	unsigned source,
	test::engine_tests::NativeRequirements native =
		test::engine_tests::NativeRequirements::baseline())
{
	const unsigned width = wide ? 64U : 32U;
	unsigned seed = 200'000U + operation * 10'000U + wide * 5'000U;
	for (unsigned lsb = 0; lsb < width; ++lsb)
		for (unsigned field_width = 1U; field_width <= width - lsb;
		     ++field_width)
			test::check(harness, name, wide, operation,
				(width - lsb) % width, field_width - 1U,
				source, 7U, test::state(++seed), native);
}

void extraction_alias_ranges(test::engine_tests::EngineHarness &harness,
	bool wide, unsigned operation, std::string_view name)
{
	const unsigned width = wide ? 64U : 32U;
	unsigned seed = 300'000U + operation * 10'000U + wide * 5'000U;
	for (unsigned lsb = 0; lsb < width; ++lsb)
		for (unsigned field_width = 1U; field_width <= width - lsb;
		     ++field_width)
			test::check(harness, name, wide, operation, lsb,
				lsb + field_width - 1U, 1U, 7U,
				test::state(++seed));
}

void every_named_alias(test::engine_tests::EngineHarness &harness)
{
	using ::uml::cpu::ArmFeature;
	const auto bfc_native =
		test::engine_tests::NativeRequirements::features(
			ArmFeature::asmv8p2);
	for (const bool wide : {false, true}) {
		const unsigned width = wide ? 64U : 32U;
		for (unsigned shift = 0; shift < width; ++shift) {
			test::check(harness, "base-asr-immediate", wide, 0U,
				shift, width - 1U, 1U, 7U,
				test::state(400'000U + shift + wide * 100U));
			test::check(harness, "base-lsl-immediate", wide, 2U,
				(width - shift) % width, width - shift - 1U,
				1U, 7U,
				test::state(401'000U + shift + wide * 100U));
			test::check(harness, "base-lsr-immediate", wide, 2U,
				shift, width - 1U, 1U, 7U,
				test::state(402'000U + shift + wide * 100U));
		}
		insertion_alias_ranges(harness, wide, 0U, "base-sbfiz", 1U);
		insertion_alias_ranges(harness, wide, 1U, "base-bfi", 1U);
		insertion_alias_ranges(harness, wide, 1U, "BASE-0037 BFC", 31U,
			bfc_native);
		insertion_alias_ranges(harness, wide, 2U, "base-ubfiz", 1U);
		extraction_alias_ranges(harness, wide, 0U, "base-sbfx");
		extraction_alias_ranges(harness, wide, 1U, "base-bfxil");
		extraction_alias_ranges(harness, wide, 2U, "base-ubfx");

		for (const unsigned imms : {7U, 15U})
			test::check(harness,
				imms == 7U ? "base-sxtb" : "base-sxth",
				wide, 0U, 0U, imms, 1U, 7U,
				test::state(410'000U + imms + wide * 100U));
	}
	test::check(harness, "base-sxtw", true, 0U, 0U, 31U, 1U, 7U,
		test::state(411'000U));
	for (const unsigned imms : {7U, 15U})
		test::check(harness,
			imms == 7U ? "base-uxtb" : "base-uxth",
			false, 2U, 0U, imms, 1U, 7U,
			test::state(412'000U + imms));
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	exhaustive_masks_and_values(harness);
	every_named_alias(harness);
	harness.print_summary("arm64-base-bitfield-behavior");
	return 0;
}
