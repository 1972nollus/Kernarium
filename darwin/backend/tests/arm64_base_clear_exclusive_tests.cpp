// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

namespace {

constexpr std::uint64_t initial_value = 0xf1e2'd3c4'b5a6'9788ULL;
constexpr std::uint64_t replacement = 0x1021'3243'5465'7687ULL;

[[nodiscard]] constexpr std::uint32_t clrex(unsigned immediate)
{
	return 0xd503'305fU | ((immediate & 15U) << 8);
}

[[nodiscard]] constexpr std::uint32_t scalar_exclusive(bool load,
	unsigned base, unsigned value, unsigned status = 31U)
{
	return 0xc800'0000U | (3U << 30) |
		(static_cast<unsigned>(load) << 22) |
		((status & 31U) << 16) | (31U << 10) |
		((base & 31U) << 5) |
		(value & 31U);
}

[[nodiscard]] arm64::CpuState state(unsigned seed)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(reg + seed) *
			 0x0101'0101'0101'0101ULL);
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed * 67U + reg * 29U + byte * 17U + 3U);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = seed & 1U;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	cpu.exclusive_address = 0x1234'5678'9abc'def0ULL + seed;
	cpu.exclusive_key = 0x1234U + seed;
	cpu.exclusive_value = initial_value ^ seed;
	cpu.exclusive_value2 = ~initial_value ^ seed;
	cpu.exclusive_version = 0x8877'6655'4433'2211ULL + seed;
	cpu.exclusive_size = static_cast<std::uint8_t>(1U << (seed & 3U));
	return cpu;
}

void clear_expected_monitor(arm64::CpuState &cpu)
{
	cpu.exclusive_address = ~std::uint64_t{0};
	cpu.exclusive_key = 0;
	cpu.exclusive_value = 0;
	cpu.exclusive_value2 = 0;
	cpu.exclusive_version = 0;
	cpu.exclusive_size = 0;
}

void every_immediate_clears_all_monitor_state(
	engine_tests::EngineHarness &harness)
{
	for (unsigned immediate = 0; immediate < 16; ++immediate) {
		const auto initial = state(immediate);
		auto expected = initial;
		clear_expected_monitor(expected);
		harness.check_one("clear-exclusive immediate", clrex(immediate),
			initial, expected,
			engine_tests::NativeRequirements::baseline());
	}
}

void scalar_monitor_is_cleared(engine_tests::EngineHarness &harness)
{
	for (unsigned immediate = 0; immediate < 16; ++immediate) {
		std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> bytes{};
		std::memcpy(bytes.data(), &initial_value, sizeof(initial_value));
		const engine_tests::MemoryRegion region{
			.address = engine_tests::engine_test_data_base,
			.initial = bytes,
		};
		const std::array code{
			engine_tests::ProgramWord{
				.offset = 0,
				.instruction = scalar_exclusive(true, 2, 4),
			},
			engine_tests::ProgramWord{
				.offset = 4,
				.instruction = clrex(immediate),
			},
			engine_tests::ProgramWord{
				.offset = 8,
				.instruction = scalar_exclusive(false, 2, 3, 5),
			},
			engine_tests::ProgramWord{
				.offset = 12,
				.instruction = 0xd400'0001U,
			},
		};
		auto initial = state(32U + immediate);
		initial.registers[2] = engine_tests::engine_test_data_base;
		initial.registers[3] = replacement;
		auto expected = initial;
		expected.registers[4] = initial_value;
		expected.registers[5] = 1;
		expected.pc = engine_tests::engine_test_code_base + 16;
		clear_expected_monitor(expected);
		harness.check_program({
			.name = "clear-exclusive scalar sequence",
			.code = code,
			.initial = initial,
			.expected = expected,
			.native = engine_tests::NativeRequirements::baseline(),
			.memory = std::span<const engine_tests::MemoryRegion>{
				&region, 1},
		});
	}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	every_immediate_clears_all_monitor_state(harness);
	scalar_monitor_is_cleared(harness);
	harness.print_summary("arm64-base-clear-exclusive");
	return 0;
}
