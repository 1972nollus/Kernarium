// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <source_location>

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

class NullMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t, std::span<std::byte>) override
	{
		return false;
	}
	[[nodiscard]] bool write(
		std::uint64_t, std::span<const std::byte>) override
	{
		return false;
	}
};

void require(bool condition,
	     const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

constexpr std::uint64_t base = 0xb000;

class Harness final {
public:
	Harness() : tlb_(std::make_unique<gadgets::Tlb>()) {}

	void check(std::uint32_t instruction, const arm64::CpuState &initial,
		   const arm64::CpuState &expected)
	{
		auto interpreted = initial;
		interpreted.pc = base;
		require(arm64::Dispatcher{}.execute(
			interpreted, memory_, instruction).reason ==
			arm64::StopReason::continued);
		require(interpreted.registers == expected.registers);
		require(interpreted.sp == expected.sp);
		if (interpreted.nzcv != expected.nzcv)
			std::fprintf(stderr, "instruction=%08x interpreted-nzcv=%08x expected-nzcv=%08x\n",
				instruction, interpreted.nzcv, expected.nzcv);
		require(interpreted.nzcv == expected.nzcv);

		const std::array stream{instruction, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(base, stream);
		require(built.complete);
		auto gadget = initial;
		gadget.pc = base;
		tlb_->prepare(0, darwin_uml_static_vm_changes(0));
		require(gadgets::execute(built.program, gadget, *tlb_) ==
			gadgets::Event::system_call);
		require(gadget.registers == expected.registers);
		require(gadget.sp == expected.sp);
		require(gadget.nzcv == expected.nzcv);
	}

	void check_undefined(std::uint32_t instruction)
	{
		arm64::CpuState cpu{};
		cpu.pc = base;
		require(arm64::Dispatcher{}.execute(cpu, memory_, instruction).reason ==
			arm64::StopReason::unsupported_instruction);
		const std::array stream{instruction, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(base, stream);
		if (built.complete) {
			tlb_->prepare(0, darwin_uml_static_vm_changes(0));
			require(gadgets::execute(built.program, cpu, *tlb_) ==
				gadgets::Event::fatal);
		}
	}

private:
	NullMemory memory_;
	std::unique_ptr<gadgets::Tlb> tlb_;
};

[[nodiscard]] constexpr std::uint64_t mask_for(bool wide)
{
	return wide ? ~std::uint64_t{0} : std::uint64_t{0xffff'ffffU};
}

[[nodiscard]] std::uint64_t rotate_element(
	std::uint64_t value, unsigned amount, unsigned width)
{
	if (width == 64)
		return std::rotr(value, amount);
	const auto mask = (std::uint64_t{1} << width) - 1;
	amount %= width;
	return ((value >> amount) | (value << ((width - amount) % width))) & mask;
}

[[nodiscard]] std::optional<std::uint64_t> decode_immediate_reference(
	bool wide, unsigned n, unsigned immr, unsigned imms)
{
	const unsigned encoded = (n << 6) | ((~imms) & 0x3fU);
	if (encoded == 0)
		return std::nullopt;
	const unsigned length = 31U - std::countl_zero(encoded);
	const unsigned element_width = 1U << length;
	const unsigned levels = element_width - 1;
	if (element_width > (wide ? 64U : 32U) || (imms & levels) == levels)
		return std::nullopt;
	const unsigned ones_count = (imms & levels) + 1;
	const auto ones = ones_count == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << ones_count) - 1;
	const auto element = rotate_element(ones, immr & levels, element_width);
	std::uint64_t result = 0;
	for (unsigned position = 0; position < (wide ? 64U : 32U);
	     position += element_width)
		result |= element << position;
	return result & mask_for(wide);
}

[[nodiscard]] constexpr std::uint32_t immediate_instruction(
	bool wide, unsigned operation, unsigned n, unsigned immr, unsigned imms,
	unsigned rn = 1, unsigned rd = 7)
{
	return 0x1200'0000U | (wide ? 0x8000'0000U : 0) |
		(operation << 29) | (n << 22) | (immr << 16) | (imms << 10) |
		(rn << 5) | rd;
}

[[nodiscard]] constexpr std::uint64_t logical_result(
	unsigned operation, std::uint64_t left, std::uint64_t right)
{
	return operation == 0 || operation == 3 ? left & right :
		operation == 1 ? left | right : left ^ right;
}

void logical_immediates(Harness &harness)
{
	for (const bool wide : {false, true})
		for (unsigned n = 0; n <= (wide ? 1U : 0U); ++n)
			for (unsigned immr = 0; immr < 64; ++immr)
				for (unsigned imms = 0; imms < 64; ++imms) {
					const auto immediate = decode_immediate_reference(
						wide, n, immr, imms);
					if (!immediate)
						continue;
					for (unsigned operation = 0; operation < 4; ++operation) {
						arm64::CpuState initial{};
						initial.nzcv = 0xf000'0000U;
						initial.registers[1] =
							0x0123'4567'89ab'cdefULL;
						auto expected = initial;
						const auto result = logical_result(operation,
							initial.registers[1], *immediate) & mask_for(wide);
						expected.registers[7] = result;
						if (operation == 3)
							expected.nzcv =
								(result >> (wide ? 63 : 31) ?
									0x8000'0000U : 0) |
								(result == 0 ? 0x4000'0000U : 0);
						harness.check(immediate_instruction(wide,
							operation, n, immr, imms), initial, expected);
					}
				}

	/* MOV (bitmask), SP destination semantics, and TST. */
	for (const bool wide : {false, true}) {
		const unsigned n = wide ? 1U : 0U;
		constexpr unsigned immr = 7;
		constexpr unsigned imms = 13;
		const auto immediate = decode_immediate_reference(wide, n, immr, imms);
		require(immediate.has_value());
		arm64::CpuState initial{};
		initial.nzcv = 0x3000'0000U;
		initial.sp = 0xfeed'face'feed'faceULL;
		auto expected = initial;
		expected.registers[7] = *immediate;
		harness.check(immediate_instruction(wide, 1, n, immr, imms, 31),
			initial, expected);
		expected = initial;
		expected.sp = logical_result(2, 0, *immediate) & mask_for(wide);
		harness.check(immediate_instruction(wide, 2, n, immr, imms, 31, 31),
			initial, expected);
		expected = initial;
		const auto result = initial.registers[1] & *immediate;
		expected.nzcv = (result >> (wide ? 63 : 31) ? 0x8000'0000U : 0) |
			(result == 0 ? 0x4000'0000U : 0);
		harness.check(immediate_instruction(wide, 3, n, immr, imms, 1, 31),
			initial, expected);
	}

	/* Reserved logical-immediate encodings. */
	harness.check_undefined(immediate_instruction(false, 0, 1, 0, 0));
	harness.check_undefined(immediate_instruction(true, 0, 1, 0, 63));
}

[[nodiscard]] constexpr std::uint32_t shifted_instruction(
	bool wide, unsigned operation, bool invert, unsigned shift_type,
	unsigned amount, unsigned rn = 1, unsigned rm = 2, unsigned rd = 7)
{
	return 0x0a00'0000U | (wide ? 0x8000'0000U : 0) |
		(operation << 29) | (shift_type << 22) |
		(invert ? 0x0020'0000U : 0) | (rm << 16) | (amount << 10) |
		(rn << 5) | rd;
}

[[nodiscard]] std::uint64_t shifted_value(
	std::uint64_t value, bool wide, unsigned type, unsigned amount)
{
	value &= mask_for(wide);
	if (type == 0)
		return (value << amount) & mask_for(wide);
	if (type == 1)
		return value >> amount;
	if (type == 2)
		return wide ? static_cast<std::uint64_t>(
			static_cast<std::int64_t>(value) >> amount) :
			static_cast<std::uint32_t>(
				static_cast<std::int32_t>(value) >> amount);
	return wide ? std::rotr(value, amount) :
		std::rotr(static_cast<std::uint32_t>(value), amount);
}

void logical_shifted(Harness &harness)
{
	const std::array pairs{
		std::array{std::uint64_t{0}, std::uint64_t{0}},
		std::array{std::uint64_t{1}, std::uint64_t{1}},
		std::array{std::uint64_t{0xaaaa'5555U}, std::uint64_t{0xf0f0'0f0fU}},
		std::array{std::uint64_t{0x0123'4567'89ab'cdefULL},
			std::uint64_t{0x8000'0000'0000'0001ULL}},
		std::array{~std::uint64_t{0}, ~std::uint64_t{0}}};
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 4; ++operation)
			for (const bool invert : {false, true})
				for (unsigned type = 0; type < 4; ++type)
					for (const unsigned amount : {0U, 1U,
						wide ? 31U : 15U, wide ? 63U : 31U})
						for (const auto &pair : pairs) {
							auto right = shifted_value(pair[1], wide,
								type, amount);
							if (invert)
								right = ~right & mask_for(wide);
							const auto result = logical_result(operation,
								pair[0], right) & mask_for(wide);
							arm64::CpuState initial{};
							initial.nzcv = 0xf000'0000U;
							initial.registers[1] = pair[0];
							initial.registers[2] = pair[1];
							auto expected = initial;
							expected.registers[7] = result;
							if (operation == 3)
								expected.nzcv =
									(result >> (wide ? 63 : 31) ?
										0x8000'0000U : 0) |
									(result == 0 ? 0x4000'0000U : 0);
							harness.check(shifted_instruction(wide,
								operation, invert, type, amount),
								initial, expected);
						}

	/* MOV (register), MVN, and TST aliases plus zero destinations. */
	for (const bool wide : {false, true}) {
		arm64::CpuState initial{};
		initial.nzcv = 0x2000'0000U;
		initial.registers[1] = ~std::uint64_t{0};
		initial.registers[2] = 0x0123'4567'89ab'cdefULL;
		auto expected = initial;
		expected.registers[7] = initial.registers[2] & mask_for(wide);
		harness.check(shifted_instruction(wide, 1, false, 0, 0, 31),
			initial, expected);
		expected.registers[7] = ~initial.registers[2] & mask_for(wide);
		harness.check(shifted_instruction(wide, 1, true, 0, 0, 31),
			initial, expected);
		expected = initial;
		const auto result = (initial.registers[1] & initial.registers[2]) &
			mask_for(wide);
		expected.nzcv = (result >> (wide ? 63 : 31) ? 0x8000'0000U : 0) |
			(result == 0 ? 0x4000'0000U : 0);
		harness.check(shifted_instruction(wide, 3, false, 0, 0, 1, 2, 31),
			initial, expected);
	}
	harness.check_undefined(shifted_instruction(false, 0, false, 0, 32));
}

} // namespace

int main()
{
	Harness harness;
	logical_immediates(harness);
	logical_shifted(harness);
	return 0;
}
