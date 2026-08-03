// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_two_register_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace test = arm64_simd_two_register_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

[[noreturn]] void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned index)
{
	return offsetof(test::arm64::CpuState, vectors) +
		index * sizeof(test::arm64::VectorRegister);
}

[[nodiscard]] constexpr std::uintptr_t packed_operand(
	test::Registers registers)
{
	return vector_offset(registers.destination) |
		(vector_offset(registers.source) << 16U);
}

void production_specialization()
{
	static_assert(vector_offset(31U) <= 0xffffU);
	constexpr auto base = test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{7U, 23U};
	std::set<std::uintptr_t> entries;
	std::size_t forms = 0;

	test::for_each_allocated_form(test::target_opcodes,
		[&](bool scalar, bool q, bool unsigned_group,
		    unsigned size, unsigned opcode) {
			const auto built = gadgets::Builder{}.build(base,
				std::array{test::instruction(scalar, q,
					unsigned_group, size, opcode,
					registers)});
			require(built.complete,
				"allocated two-register instruction did not build");
			require(built.program.words().size() == 4U,
				"two-register gadget stream has an unexpected shape");
			require(built.program.words()[1] ==
					packed_operand(registers),
				"register fields were not translated to direct CpuState offsets");
			entries.insert(built.program.words().front());
			++forms;
		});

	require(forms == 193U,
		"two-register allocation did not produce 193 native forms");
	require(entries.size() == forms,
		"operation or arrangement retained a shared runtime decoder");
}

} // namespace

int main()
{
	production_specialization();
	return 0;
}
