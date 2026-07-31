// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_literal_load_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <vector>

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;
namespace literal_test = arm64_literal_load_test;

namespace {

constexpr std::uint32_t terminal_system_call = 0xd400'0001U;
constexpr std::size_t batch_size = 63;

[[nodiscard]] constexpr auto offset_samples()
{
	constexpr unsigned page_words =
		static_cast<unsigned>(literal_test::page_size / 4U);
	return std::array{
		0x40000U,
		0x40001U,
		0x55555U,
		0x80000U - page_words - 1U,
		0x80000U - page_words,
		page_words,
		page_words + 1U,
		0x15555U,
		0x2aaaaU,
		0x3ffffU,
	};
}

void allocated_load_matrix(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < literal_test::load_variants.size();
	     ++variant_index) {
		const auto &variant = literal_test::load_variants[variant_index];
		for (const auto imm19 : offset_samples())
			for (unsigned destination = 0; destination < 32;
			     ++destination) {
				const auto address =
					literal_test::target(literal_test::code_base, imm19);
				auto initial = literal_test::seeded_state(
					0x1000'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant_index) << 32) |
					(static_cast<std::uint64_t>(imm19) << 5) |
					destination);
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s allocated imm19=%#x rt=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(), imm19, destination);
				harness.check({
					.name = name,
					.instruction = literal_test::literal(
						variant.vector, variant.operation, imm19,
						destination),
					.initial = initial,
					.expected = initial,
					.event = engine_tests::ExpectedEvent::memory_fault,
					.native = literal_test::native(variant),
					.fault = {
						.address = address,
						.access = arm64::MemoryAccess::read,
					},
					.compare_fault = true,
				});
			}
	}
}

void exhaustive_prefetch_offsets(engine_tests::EngineHarness &harness)
{
	/*
	 * Document pages 592-593 allocate every Rt value. Rt=0..23 include the
	 * named spellings (including feature-dependent names); the remaining
	 * values and unavailable names use the equivalent immediate spelling.
	 * A hint is allowed to have no observable effect and must not access the
	 * target address.
	 */
	std::vector<std::uint32_t> stream;
	stream.reserve(1U << 19);
	for (unsigned imm19 = 0; imm19 < (1U << 19); ++imm19)
		stream.push_back(literal_test::literal(
			false, 3, imm19, imm19 & 31U));

	for (std::size_t first = 0; first < stream.size();
	     first += batch_size) {
		const auto count =
			std::min(batch_size, stream.size() - first);
		std::vector<engine_tests::ProgramWord> code;
		code.reserve(count + 1U);
		for (std::size_t index = 0; index < count; ++index)
			code.push_back({
				.offset = index * sizeof(std::uint32_t),
				.instruction = stream[first + index],
			});
		code.push_back({
			.offset = count * sizeof(std::uint32_t),
			.instruction = terminal_system_call,
		});
		const auto initial = literal_test::seeded_state(
			0x2000'0000'0000'0000ULL | first);
		auto expected = initial;
		expected.pc = literal_test::code_base +
			code.size() * sizeof(std::uint32_t);
		harness.check_program({
			.name = "prfm-literal exhaustive imm19",
			.code = std::span<const engine_tests::ProgramWord>{code},
			.initial = initial,
			.expected = expected,
			.native = engine_tests::NativeRequirements::baseline(),
		});
	}

	for (unsigned operation = 0; operation < 32; ++operation)
		for (const auto imm19 : offset_samples()) {
			const auto initial = literal_test::seeded_state(
				0x2100'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(imm19) << 5) |
				operation);
			char name[96];
			std::snprintf(name, sizeof(name),
				"prfm-literal operation=%u imm19=%#x",
				operation, imm19);
			harness.check_one(name,
				literal_test::literal(false, 3, imm19, operation),
				initial, initial,
				engine_tests::NativeRequirements::baseline());
		}
}

void exhaustive_reserved_offsets(engine_tests::EngineHarness &harness)
{
	const auto initial =
		literal_test::seeded_state(0x3000'0000'0000'0000ULL);
	for (unsigned imm19 = 0; imm19 < (1U << 19); ++imm19) {
		char name[80];
		std::snprintf(name, sizeof(name),
			"reserved-vector-opc-11 imm19=%#x", imm19);
		harness.check_illegal(name,
			literal_test::literal(true, 3, imm19, 0), initial,
			engine_tests::NativeRequirements::features(
				uml::backend::ArmFeature::floating_point));
	}
	for (unsigned destination = 1; destination < 32; ++destination)
		for (const auto imm19 : offset_samples()) {
			char name[96];
			std::snprintf(name, sizeof(name),
				"reserved-vector-opc-11 imm19=%#x rt=%u",
				imm19, destination);
			harness.check_illegal(name,
				literal_test::literal(true, 3, imm19, destination),
				initial,
				engine_tests::NativeRequirements::features(
					uml::backend::ArmFeature::floating_point));
		}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	allocated_load_matrix(harness);
	exhaustive_prefetch_offsets(harness);
	exhaustive_reserved_offsets(harness);
	harness.print_summary("arm64-literal-load-allocation");
	return 0;
}
