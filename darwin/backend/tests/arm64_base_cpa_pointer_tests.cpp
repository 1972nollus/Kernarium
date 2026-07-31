// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <source_location>

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x5e0'0000;

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

[[nodiscard]] constexpr std::uint32_t pointer_instruction(bool subtract,
	unsigned destination, unsigned base, unsigned offset, unsigned shift)
{
	return (subtract ? 0xda00'2000U : 0x9a00'2000U) |
		((offset & 31U) << 16) | ((shift & 7U) << 10) |
		((base & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint64_t pointer_check(bool enabled,
	std::uint64_t result, std::uint64_t base)
{
	const bool previous = ((base >> 55) & 1U) != ((base >> 54) & 1U);
	const bool detected = (result >> 56) != (base >> 56) || previous;
	if (!enabled || !detected)
		return result;
	constexpr auto low_mask = (std::uint64_t{1} << 54) - 1;
	return (result & low_mask) | (base & 0xff80'0000'0000'0000ULL) |
		(((~base >> 55) & 1U) << 54);
}

[[nodiscard]] arm64::CpuState state(bool enabled = true)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			(static_cast<std::uint64_t>(0x20U + index * 7U) << 56) |
			(0x0055'aa00'1234'0000ULL +
			 static_cast<std::uint64_t>(index) * 0x1011ULL);
	cpu.sp = 0xef80'0000'0000'0100ULL;
	cpu.nzcv = 0xd000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.set_mte_configuration(true, 0x8421U);
	cpu.set_mte_generator(false, 0xace1U, 7);
	cpu.cpa_enabled = enabled;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0xa1b2'c3d4U;
	return cpu;
}

[[nodiscard]] arm64::CpuState expected_result(const arm64::CpuState &initial,
	bool subtract, unsigned destination, unsigned base_register,
	unsigned offset_register, unsigned shift)
{
	auto expected = initial;
	const auto base = base_register == 31 ? initial.sp :
		initial.registers[base_register];
	const auto offset = offset_register == 31 ? 0 :
		initial.registers[offset_register] << shift;
	const auto arithmetic = subtract ? base - offset : base + offset;
	const auto result = pointer_check(initial.cpa_enabled, arithmetic, base);
	if (destination == 31)
		expected.sp = result;
	else
		expected.registers[destination] = result;
	return expected;
}

class Harness final {
public:
	void check(bool subtract, unsigned destination, unsigned base,
		unsigned offset, unsigned shift, const arm64::CpuState &initial)
	{
		const auto instruction = pointer_instruction(
			subtract, destination, base, offset, shift);
		const auto expected = expected_result(
			initial, subtract, destination, base, offset, shift);
		engines_.check_one("checked pointer add/subtract", instruction,
			initial, expected,
			engine_tests::NativeRequirements::features(
				uml::backend::ArmFeature::cpa));
	}

	void print_summary() const
	{
		engines_.print_summary("arm64-base-cpa-pointer");
	}

private:
	engine_tests::EngineHarness engines_;
};

void encoding_examples()
{
	require(pointer_instruction(false, 0, 1, 2, 0) == 0x9a02'2020U);
	require(pointer_instruction(false, 31, 31, 31, 7) == 0x9a1f'3fffU);
	require(pointer_instruction(true, 30, 1, 2, 3) == 0xda02'2c3eU);
	require(pointer_instruction(true, 31, 31, 31, 7) == 0xda1f'3fffU);
}

void exhaustive_encodings(Harness &harness)
{
	const auto initial = state(true);
	for (const bool subtract : {false, true})
		for (unsigned shift = 0; shift < 8; ++shift)
			for (unsigned destination = 0; destination < 32; ++destination)
				for (unsigned base = 0; base < 32; ++base)
					for (unsigned offset = 0; offset < 32; ++offset)
						harness.check(subtract, destination, base,
							offset, shift, initial);
}

void detection_and_disabled_edges(Harness &harness)
{
	constexpr std::array bases{
		std::uint64_t{0}, std::uint64_t{0x003f'ffff'ffff'ffffULL},
		std::uint64_t{0x0040'0000'0000'0000ULL},
		std::uint64_t{0x0080'0000'0000'0000ULL},
		std::uint64_t{0x7f00'0000'0000'0000ULL},
		std::uint64_t{0x80ff'ffff'ffff'ffffULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL}};
	constexpr std::array offsets{
		std::uint64_t{0}, std::uint64_t{1},
		std::uint64_t{0x00ff'ffff'ffff'ffffULL},
		std::uint64_t{0x0100'0000'0000'0000ULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL}};
	for (const bool enabled : {false, true})
		for (const auto base : bases)
			for (const auto offset : offsets)
				for (const bool subtract : {false, true}) {
					auto initial = state(enabled);
					initial.registers[1] = base;
					initial.registers[2] = offset;
					for (unsigned shift = 0; shift < 8; ++shift)
						harness.check(subtract, 3, 1, 2, shift, initial);
				}
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool subtract : {false, true})
		for (unsigned shift = 0; shift < 8; ++shift)
			for (const bool base_sp : {false, true})
				for (const bool offset_zero : {false, true})
					for (const bool destination_sp : {false, true}) {
						const auto built = gadgets::Builder{}.build(pc,
							std::array{pointer_instruction(subtract,
								destination_sp ? 31U : 3U,
								base_sp ? 31U : 1U,
								offset_zero ? 31U : 2U, shift)});
						require(built.complete &&
							built.program.instruction_count() == 1 &&
							built.program.words().size() == 4);
						entries.insert(built.program.words().front());
					}
	require(entries.size() == 128);
}

} // namespace

int main()
{
	encoding_examples();
	Harness harness;
	exhaustive_encodings(harness);
	detection_and_disabled_edges(harness);
	production_specialization();
	harness.print_summary();
	return 0;
}
