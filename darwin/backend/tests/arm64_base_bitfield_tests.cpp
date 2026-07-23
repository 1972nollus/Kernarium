// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
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

constexpr std::uint64_t base = 0xd000;

[[nodiscard]] constexpr std::uint64_t mask_for(bool wide)
{
	return wide ? ~std::uint64_t{0} : std::uint64_t{0xffff'ffffU};
}

[[nodiscard]] constexpr std::uint64_t low_ones(unsigned count)
{
	return count == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << count) - 1;
}

[[nodiscard]] std::uint64_t reference(
	bool wide, unsigned operation, unsigned immr, unsigned imms,
	std::uint64_t source, std::uint64_t destination)
{
	const unsigned width = wide ? 64 : 32;
	const auto width_mask = mask_for(wide);
	source &= width_mask;
	destination &= width_mask;
	unsigned count;
	unsigned position;
	std::uint64_t field;
	if (imms >= immr) {
		count = imms - immr + 1;
		position = 0;
		field = (source >> immr) & low_ones(count);
	} else {
		count = imms + 1;
		position = width - immr;
		field = (source & low_ones(count)) << position;
	}
	if (operation == 1) {
		const auto field_mask = low_ones(count) << position;
		return ((destination & ~field_mask) | field) & width_mask;
	}
	if (operation == 2)
		return field & width_mask;
	const bool sign = ((source >> imms) & 1U) != 0;
	const unsigned top = position + count;
	if (sign && top < width)
		field |= width_mask & ~low_ones(top);
	return field & width_mask;
}

[[nodiscard]] constexpr std::uint32_t instruction(
	bool wide, unsigned operation, unsigned immr, unsigned imms,
	unsigned rn = 1, unsigned rd = 7, bool n_override = false)
{
	const bool n = n_override ? !wide : wide;
	return 0x1300'0000U | (wide ? 0x8000'0000U : 0) |
		(operation << 29) | (n ? 0x0040'0000U : 0) |
		(immr << 16) | (imms << 10) | (rn << 5) | rd;
}

class Harness final {
public:
	Harness() : tlb_(std::make_unique<gadgets::Tlb>()) {}

	void check(std::uint32_t encoded, const arm64::CpuState &initial,
		   std::uint64_t expected, unsigned destination = 7)
	{
		auto interpreted = initial;
		interpreted.pc = base;
		require(arm64::Dispatcher{}.execute(
			interpreted, memory_, encoded).reason ==
			arm64::StopReason::continued);
		if (destination != 31)
			require(interpreted.registers[destination] == expected);
		require(interpreted.nzcv == initial.nzcv);
		require(interpreted.sp == initial.sp);

		const std::array stream{encoded, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(base, stream);
		require(built.complete);
		auto gadget = initial;
		gadget.pc = base;
		tlb_->prepare(0, darwin_uml_static_vm_changes(0));
		require(gadgets::execute(built.program, gadget, *tlb_) ==
			gadgets::Event::system_call);
		if (destination != 31)
			require(gadget.registers[destination] == expected);
		require(gadget.nzcv == initial.nzcv);
		require(gadget.sp == initial.sp);
	}

	void check_undefined(std::uint32_t encoded)
	{
		arm64::CpuState cpu{};
		cpu.pc = base;
		require(arm64::Dispatcher{}.execute(cpu, memory_, encoded).reason ==
			arm64::StopReason::unsupported_instruction);
		const std::array stream{encoded, 0xd400'0001U};
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

void exhaustive_bitfields(Harness &harness)
{
	const std::array pairs{
		std::array{std::uint64_t{0}, std::uint64_t{0}},
		std::array{~std::uint64_t{0}, std::uint64_t{0}},
		std::array{std::uint64_t{0}, ~std::uint64_t{0}},
		std::array{std::uint64_t{0x0123'4567'89ab'cdefULL},
			std::uint64_t{0xfedc'ba98'7654'3210ULL}},
		std::array{std::uint64_t{0x8000'0000'0000'0001ULL},
			std::uint64_t{0xaaaa'5555'aaaa'5555ULL}}};
	for (const bool wide : {false, true}) {
		const unsigned width = wide ? 64 : 32;
		for (unsigned operation = 0; operation < 3; ++operation)
			for (unsigned immr = 0; immr < width; ++immr)
				for (unsigned imms = 0; imms < width; ++imms)
					for (const auto &pair : pairs) {
						arm64::CpuState initial{};
						initial.nzcv = 0xf000'0000U;
						initial.sp = 0xfeed'face'feed'faceULL;
						initial.registers[1] = pair[0];
						initial.registers[7] = pair[1];
						const auto expected = reference(wide,
							operation, immr, imms, pair[0], pair[1]);
						harness.check(instruction(wide, operation,
							immr, imms), initial, expected);
					}
	}
}

void aliases_and_zero_registers(Harness &harness)
{
	/* ASR/SBFIZ/SBFX/SXTB/SXTH/SXTW; BFC/BFI/BFXIL; and
	 * LSL/LSR/UBFIZ/UBFX/UXTB/UXTH are encodings of these three operations.
	 */
	for (const bool wide : {false, true}) {
		const unsigned width = wide ? 64 : 32;
		const std::array encodings{
			std::array{0U, 5U, width - 1},       // ASR
			std::array{0U, width - 9, 7U},       // SBFIZ
			std::array{0U, 7U, 15U},             // SBFX
			std::array{0U, 0U, 7U},              // SXTB
			std::array{0U, 0U, 15U},             // SXTH
			std::array{1U, width - 9, 7U},       // BFI
			std::array{1U, 7U, 15U},             // BFXIL
			std::array{2U, width - 5, width - 6}, // LSL
			std::array{2U, 5U, width - 1},       // LSR
			std::array{2U, width - 9, 7U},       // UBFIZ
			std::array{2U, 7U, 15U},             // UBFX
			std::array{2U, 0U, 7U},              // UXTB
			std::array{2U, 0U, 15U},             // UXTH
		};
		for (const auto &encoding : encodings) {
			arm64::CpuState initial{};
			initial.nzcv = 0xa000'0000U;
			initial.sp = 0xdead'beef'dead'beefULL;
			initial.registers[1] = 0x89ab'cdef'0123'4567ULL;
			initial.registers[7] = 0xfedc'ba98'7654'3210ULL;
			const auto expected = reference(wide, encoding[0], encoding[1],
				encoding[2], initial.registers[1], initial.registers[7]);
			harness.check(instruction(wide, encoding[0], encoding[1],
				encoding[2]), initial, expected);
		}

		/* BFC has Rn==31; same-register source/destination must use old state. */
		arm64::CpuState initial{};
		initial.nzcv = 0x5000'0000U;
		initial.registers[7] = 0xffff'ffff'ffff'ffffULL;
		const unsigned immr = width - 8;
		const unsigned imms = 7;
		auto expected = reference(wide, 1, immr, imms, 0,
			initial.registers[7]);
		harness.check(instruction(wide, 1, immr, imms, 31), initial,
			expected);
		initial.registers[1] = 0x0123'4567'89ab'cdefULL;
		expected = reference(wide, 1, 3, 27, initial.registers[1],
			initial.registers[1]);
		harness.check(instruction(wide, 1, 3, 27, 1, 1), initial,
			expected, 1);
		harness.check(instruction(wide, 2, 0, 7, 1, 31), initial,
			0, 31);
	}

	/* SXTW exists only in the 64-bit SBFM form. */
	arm64::CpuState initial{};
	initial.nzcv = 0x6000'0000U;
	initial.registers[1] = 0x0000'0000'8000'0001ULL;
	harness.check(instruction(true, 0, 0, 31), initial,
		reference(true, 0, 0, 31, initial.registers[1], 0));
}

void undefined_encodings(Harness &harness)
{
	harness.check_undefined(instruction(true, 3, 0, 0));
	harness.check_undefined(instruction(true, 0, 0, 0, 1, 7, true));
	harness.check_undefined(instruction(false, 0, 0, 0, 1, 7, true));
	harness.check_undefined(instruction(false, 0, 32, 0));
	harness.check_undefined(instruction(false, 2, 0, 32));
}

} // namespace

int main()
{
	Harness harness;
	exhaustive_bitfields(harness);
	aliases_and_zero_registers(harness);
	undefined_encodings(harness);
	return 0;
}
