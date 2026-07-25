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

constexpr std::uint64_t base = 0x1234'5678'9abcULL;

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
		require(interpreted.pc == expected.pc);
		require(interpreted.nzcv == expected.nzcv);
		require(interpreted.sp == expected.sp);

		const std::array stream{instruction};
		const auto built = gadgets::Builder{}.build(base, stream);
		require(built.complete);
		auto gadget = initial;
		gadget.pc = base;
		tlb_->prepare(0, darwin_uml_static_vm_changes(0));
		const auto event = gadgets::execute(built.program, gadget, *tlb_);
		if (event != gadgets::Event::continued &&
		    event != gadgets::Event::interrupt)
			std::fprintf(stderr,
				"control gadget mismatch instruction=%#x event=%d\n",
				instruction, static_cast<int>(event));
		require(event == gadgets::Event::continued ||
			event == gadgets::Event::interrupt);
		require(gadget.registers == expected.registers);
		require(gadget.pc == expected.pc);
		require(gadget.nzcv == expected.nzcv);
		require(gadget.sp == expected.sp);
	}

private:
	NullMemory memory_;
	std::unique_ptr<gadgets::Tlb> tlb_;
};

[[nodiscard]] constexpr bool condition_holds(
	std::uint32_t nzcv, unsigned condition)
{
	const bool n = (nzcv & 0x8000'0000U) != 0;
	const bool z = (nzcv & 0x4000'0000U) != 0;
	const bool c = (nzcv & 0x2000'0000U) != 0;
	const bool v = (nzcv & 0x1000'0000U) != 0;
	bool result = false;
	switch (condition >> 1) {
	case 0: result = z; break;
	case 1: result = c; break;
	case 2: result = n; break;
	case 3: result = v; break;
	case 4: result = c && !z; break;
	case 5: result = n == v; break;
	case 6: result = !z && n == v; break;
	case 7: result = true; break;
	default: break;
	}
	return (condition & 1U) != 0 && condition < 14 ? !result : result;
}

[[nodiscard]] constexpr std::uint32_t pc_relative_instruction(
	bool page, std::int64_t immediate, unsigned rd = 7)
{
	const auto encoded = static_cast<std::uint64_t>(immediate) & 0x1f'ffffU;
	return (page ? 0x9000'0000U : 0x1000'0000U) |
		((encoded & 3U) << 29) | ((encoded >> 2) << 5) | rd;
}

void pc_relative(Harness &harness)
{
	const std::array<std::int64_t, 7> immediates{
		-(std::int64_t{1} << 20), -4097, -1, 0, 1, 4095,
		(std::int64_t{1} << 20) - 1};
	for (const bool page : {false, true})
		for (const auto immediate : immediates) {
			arm64::CpuState initial{};
			initial.nzcv = 0xf000'0000U;
			auto expected = initial;
			expected.pc = base + 4;
			expected.registers[7] = page ?
				static_cast<std::uint64_t>(
					static_cast<std::int64_t>(base & ~0xfffULL) +
					immediate * 4096) :
				static_cast<std::uint64_t>(
					static_cast<std::int64_t>(base) + immediate);
			harness.check(pc_relative_instruction(page, immediate),
				initial, expected);
		}
	arm64::CpuState initial{};
	initial.nzcv = 0x5000'0000U;
	auto expected = initial;
	expected.pc = base + 4;
	harness.check(pc_relative_instruction(false, 123, 31), initial, expected);
}

[[nodiscard]] constexpr std::uint32_t branch_immediate_instruction(
	bool link, std::int64_t displacement)
{
	const auto immediate = (static_cast<std::uint64_t>(displacement) >> 2) &
		0x03ff'ffffU;
	return (link ? 0x9400'0000U : 0x1400'0000U) | immediate;
}

void direct_branches(Harness &harness)
{
	const std::array<std::int64_t, 7> displacements{
		-(std::int64_t{1} << 27), -4096, -4, 0, 4, 4096,
		(std::int64_t{1} << 27) - 4};
	for (const bool link : {false, true})
		for (const auto displacement : displacements) {
			arm64::CpuState initial{};
			initial.nzcv = 0xa000'0000U;
			initial.registers[30] = 0xfeed'face;
			auto expected = initial;
			expected.pc = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(base) + displacement);
			if (link)
				expected.registers[30] = base + 4;
			harness.check(branch_immediate_instruction(link, displacement),
				initial, expected);
		}
}

[[nodiscard]] constexpr std::uint32_t conditional_branch_instruction(
	bool consistent, unsigned condition, std::int64_t displacement)
{
	const auto immediate = (static_cast<std::uint64_t>(displacement) >> 2) &
		0x7ffffU;
	return 0x5400'0000U | (immediate << 5) |
		(consistent ? 0x10U : 0) | condition;
}

void conditional_branches(Harness &harness)
{
	for (const bool consistent : {false, true})
		for (unsigned condition = 0; condition < 16; ++condition)
			for (unsigned flags = 0; flags < 16; ++flags)
				for (const std::int64_t displacement : {-1'048'576, -4, 8,
					1'048'572}) {
					arm64::CpuState initial{};
					initial.nzcv = flags << 28;
					auto expected = initial;
					expected.pc = condition_holds(initial.nzcv, condition) ?
						static_cast<std::uint64_t>(
							static_cast<std::int64_t>(base) + displacement) :
						base + 4;
					harness.check(conditional_branch_instruction(consistent,
						condition, displacement), initial, expected);
				}
}

[[nodiscard]] constexpr std::uint32_t compare_branch_instruction(
	bool wide, bool nonzero, std::int64_t displacement, unsigned rt = 7)
{
	const auto immediate = (static_cast<std::uint64_t>(displacement) >> 2) &
		0x7ffffU;
	return 0x3400'0000U | (wide ? 0x8000'0000U : 0) |
		(nonzero ? 0x0100'0000U : 0) | (immediate << 5) | rt;
}

void compare_branches(Harness &harness)
{
	const std::array values{
		std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x1'0000'0000ULL},
		~std::uint64_t{0}};
	for (const bool wide : {false, true})
		for (const bool nonzero : {false, true})
			for (const auto value : values)
				for (const std::int64_t displacement : {-1'048'576, 8,
					1'048'572}) {
					arm64::CpuState initial{};
					initial.nzcv = 0xf000'0000U;
					initial.registers[7] = value;
					const auto tested = value & (wide ?
						~std::uint64_t{0} : std::uint64_t{0xffff'ffffU});
					const bool take = nonzero ? tested != 0 : tested == 0;
					auto expected = initial;
					expected.pc = take ? static_cast<std::uint64_t>(
						static_cast<std::int64_t>(base) + displacement) :
						base + 4;
					harness.check(compare_branch_instruction(wide, nonzero,
						displacement), initial, expected);
				}
}

[[nodiscard]] constexpr std::uint32_t test_branch_instruction(
	bool nonzero, unsigned bit, std::int64_t displacement, unsigned rt = 7)
{
	const auto immediate = (static_cast<std::uint64_t>(displacement) >> 2) &
		0x3fffU;
	return 0x3600'0000U | ((bit >> 5) << 31) |
		(nonzero ? 0x0100'0000U : 0) | ((bit & 31U) << 19) |
		(immediate << 5) | rt;
}

void test_branches(Harness &harness)
{
	for (const bool nonzero : {false, true})
		for (unsigned bit = 0; bit < 64; ++bit)
			for (const bool set : {false, true}) {
				arm64::CpuState initial{};
				initial.nzcv = 0x6000'0000U;
				initial.registers[7] = set ? std::uint64_t{1} << bit : 0;
				const bool take = nonzero ? set : !set;
				auto expected = initial;
				expected.pc = take ? base - 32'768 : base + 4;
				harness.check(test_branch_instruction(nonzero, bit, -32'768),
					initial, expected);
			}
}

[[nodiscard]] constexpr std::uint32_t register_branch_instruction(
	unsigned operation, unsigned rn)
{
	return (operation == 0 ? 0xd61f'0000U : operation == 1 ?
		0xd63f'0000U : 0xd65f'0000U) | (rn << 5);
}

void register_branches(Harness &harness)
{
	for (unsigned operation = 0; operation < 3; ++operation)
		for (const unsigned rn : {7U, 30U, 31U}) {
			arm64::CpuState initial{};
			initial.nzcv = 0x9000'0000U;
			initial.sp = 0xdead'beef'dead'beefULL;
			initial.registers[7] = 0x1234'5678;
			initial.registers[30] = 0x8765'4321;
			auto expected = initial;
			expected.pc = rn == 31 ? 0 : initial.registers[rn];
			if (operation == 1)
				expected.registers[30] = base + 4;
			harness.check(register_branch_instruction(operation, rn),
				initial, expected);
		}
}

} // namespace

int main()
{
	Harness harness;
	pc_relative(harness);
	direct_branches(harness);
	conditional_branches(harness);
	compare_branches(harness);
	test_branches(harness);
	register_branches(harness);
	return 0;
}
