// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/dispatcher.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>

namespace arm64 = uml::backend::arm64;
namespace detail = uml::backend::arm64::detail;

namespace {

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

class UnusedMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(
		std::uint64_t, std::span<std::byte>) override
	{
		return false;
	}

	[[nodiscard]] bool write(
		std::uint64_t, std::span<const std::byte>) override
	{
		return false;
	}
};

void set_single(arm64::CpuState &cpu, unsigned reg, std::uint32_t bits)
{
	for (unsigned byte = 0; byte < 4; ++byte)
		cpu.vectors[reg].bytes[byte] =
			static_cast<std::byte>(bits >> (byte * 8U));
}

[[nodiscard]] std::uint32_t get_single(
	const arm64::CpuState &cpu, unsigned reg)
{
	std::uint32_t bits{};
	for (unsigned byte = 0; byte < 4; ++byte)
		bits |= static_cast<std::uint32_t>(
			std::to_integer<unsigned>(cpu.vectors[reg].bytes[byte]))
			<< (byte * 8U);
	return bits;
}

[[nodiscard]] arm64::CpuState populated_state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			0x1000'0000'0000'0000ULL + index * 0x0101'0101ULL;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < 16; ++byte)
			cpu.vectors[reg].bytes[byte] =
				static_cast<std::byte>(reg * 17U + byte);
	cpu.sp = 0xface'1234'5678'0000ULL;
	cpu.pc = 0x4000;
	cpu.nzcv = 0xa000'0000U;
	cpu.tls = 0xcafe'babe'dead'beefULL;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	return cpu;
}

void require_trap_rollback(const arm64::CpuState &actual,
	const arm64::CpuState &initial, std::uint32_t expected_fpsr)
{
	require(actual.registers == initial.registers);
	require(actual.sp == initial.sp);
	require(actual.pc == initial.pc);
	require(actual.nzcv == initial.nzcv);
	for (unsigned reg = 0; reg < actual.vectors.size(); ++reg)
		require(actual.vectors[reg].bytes == initial.vectors[reg].bytes);
	require(actual.fpcr == initial.fpcr);
	require(actual.fpsr == expected_fpsr);
	require(actual.tls == initial.tls);
	require(actual.fpmr == initial.fpmr);
}

void enabled_cause_traps_without_committing_destination()
{
	constexpr std::uint32_t fdiv_s0_s1_s2 = 0x1e22'1820U;
	constexpr std::uint32_t divide_by_zero = 1U << 1U;
	UnusedMemory memory;
	auto cpu = populated_state();
	cpu.fpcr = 1U << 9U;
	cpu.fpsr = (1U << 27U) | (1U << 7U) | (1U << 0U);
	set_single(cpu, 1, 0x3f80'0000U);
	set_single(cpu, 2, 0x0000'0000U);
	const auto initial = cpu;

	const auto result =
		arm64::Dispatcher{}.execute(cpu, memory, fdiv_s0_s1_s2);

	require(result.reason == arm64::StopReason::floating_point_trap);
	require(result.address == initial.pc);
	require(result.instruction == fdiv_s0_s1_s2);
	require(result.instructions == 0U);
	require(result.syndrome ==
		detail::floating_exception_syndrome(divide_by_zero));
	require_trap_rollback(cpu, initial, initial.fpsr);
}

void deferred_fp_state_uses_deferred_trap_enables()
{
	constexpr std::uint32_t fdiv_s0_s1_s2 = 0x1e22'1820U;
	constexpr std::uint32_t divide_by_zero = 1U << 1U;
	UnusedMemory memory;
	auto cpu = populated_state();
	auto deferred_vectors = cpu.vectors;
	for (unsigned byte = 0; byte < 4; ++byte) {
		deferred_vectors[1].bytes[byte] = static_cast<std::byte>(
			0x3f80'0000U >> (byte * 8U));
		deferred_vectors[2].bytes[byte] = std::byte{};
	}
	constexpr std::uint32_t deferred_fpcr = 1U << 9U;
	constexpr std::uint32_t deferred_fpsr = (1U << 27U) | (1U << 7U);
	cpu.fpcr = 0U;
	cpu.fpsr = 0U;
	cpu.defer_fp_load(
		deferred_vectors.data(), deferred_fpcr, deferred_fpsr);
	const auto original_pc = cpu.pc;

	const auto result =
		arm64::Dispatcher{}.execute(cpu, memory, fdiv_s0_s1_s2);

	require(result.reason == arm64::StopReason::floating_point_trap);
	require(result.address == original_pc);
	require(result.syndrome ==
		detail::floating_exception_syndrome(divide_by_zero));
	require(cpu.fp_loaded);
	require(cpu.fpcr == deferred_fpcr);
	require(cpu.fpsr == deferred_fpsr);
	require(cpu.pc == original_pc);
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		require(cpu.vectors[reg].bytes == deferred_vectors[reg].bytes);
}

void trap_commits_only_newly_raised_unenabled_flags()
{
	constexpr std::uint32_t fmul_s0_s1_s2 = 0x1e22'0820U;
	constexpr std::uint32_t overflow = 1U << 2U;
	constexpr std::uint32_t inexact = 1U << 4U;
	UnusedMemory memory;
	auto cpu = populated_state();
	cpu.fpcr = 1U << 10U;
	cpu.fpsr = 1U << 7U;
	set_single(cpu, 1, 0x7f7f'ffffU);
	set_single(cpu, 2, 0x4000'0000U);
	const auto initial = cpu;

	const auto result =
		arm64::Dispatcher{}.execute(cpu, memory, fmul_s0_s1_s2);

	require(result.reason == arm64::StopReason::floating_point_trap);
	require(result.syndrome ==
		detail::floating_exception_syndrome(overflow));
	require_trap_rollback(cpu, initial, initial.fpsr | inexact);
}

void unenabled_cause_commits_the_instruction()
{
	constexpr std::uint32_t fdiv_s0_s1_s2 = 0x1e22'1820U;
	constexpr std::uint32_t divide_by_zero = 1U << 1U;
	UnusedMemory memory;
	auto cpu = populated_state();
	cpu.fpcr = 1U << 8U;
	cpu.fpsr = 1U << 7U;
	set_single(cpu, 1, 0x3f80'0000U);
	set_single(cpu, 2, 0x0000'0000U);
	const auto original_pc = cpu.pc;

	const auto result =
		arm64::Dispatcher{}.execute(cpu, memory, fdiv_s0_s1_s2);

	require(result.reason == arm64::StopReason::continued);
	require(result.address == original_pc);
	require(result.instructions == 1U);
	require(cpu.pc == original_pc + 4U);
	require(cpu.fpsr == ((1U << 7U) | divide_by_zero));
	require(get_single(cpu, 0) == 0x7f80'0000U);
	for (unsigned byte = 4; byte < 16; ++byte)
		require(cpu.vectors[0].bytes[byte] == std::byte{});
}

void reserved_encoding_remains_illegal_with_enables()
{
	/* size==00 is reserved in this Advanced-SIMD data-processing class. */
	constexpr std::uint32_t reserved = 0x2e00'fc00U;
	UnusedMemory memory;
	auto cpu = populated_state();
	cpu.fpcr = detail::fpcr_exception_enable_mask;
	cpu.fpsr = (1U << 27U) | (1U << 7U);
	const auto initial = cpu;

	const auto result = arm64::Dispatcher{}.execute(cpu, memory, reserved);

	require(result.reason == arm64::StopReason::illegal_instruction);
	require(result.address == initial.pc);
	require(result.instruction == reserved);
	require(result.syndrome == 0U);
	require_trap_rollback(cpu, initial, initial.fpsr);
}

static_assert(detail::host_fpcr(0xffff'ffffU) ==
	(0xffff'ffffU & ~detail::fpcr_exception_enable_mask));
static_assert(detail::enabled_exception_flags(0x0000'9f00U) == 0x9fU);
static_assert(detail::is_fp_simd_data_processing(0x1e22'1820U));
static_assert(!detail::is_fp_simd_data_processing(0x3d40'0000U));

} // namespace

int main()
{
	enabled_cause_traps_without_committing_destination();
	deferred_fp_state_uses_deferred_trap_enables();
	trap_commits_only_newly_raised_unenabled_flags();
	unenabled_cause_commits_the_instruction();
	reserved_encoding_remains_illegal_with_enables();
	return 0;
}
