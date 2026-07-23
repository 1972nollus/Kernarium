// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <array>

namespace test = arm64_simd_test;

namespace {

constexpr std::array integer_opcodes{2U, 3U, 4U, 6U, 7U, 11U, 18U, 19U,
	20U};

[[nodiscard]] constexpr std::uint32_t two_register(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode,
	unsigned destination = 0, unsigned source = 1)
{
	return (scalar ? 0x5e20'0800U : 0x0e20'0800U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((opcode & 31U) << 12) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr bool valid_integer(bool scalar, bool q,
	bool unsigned_group, unsigned size, unsigned opcode)
{
	switch (opcode) {
	case 2:
	case 4:
	case 6:
		return !scalar && size < 3U;
	case 3:
	case 7:
		return scalar || q || size < 3U;
	case 11:
		return scalar ? size == 3U : q || size < 3U;
	case 18:
		return size < 3U && (unsigned_group || !scalar);
	case 19:
		return size < 3U && unsigned_group && !scalar;
	case 20:
		return size < 3U;
	default:
		return false;
	}
}

void integer_allocation()
{
	test::Harness harness;
	for (const bool scalar : {false, true})
		for (const bool q : {false, true}) {
			if (scalar && !q)
				continue;
			for (const bool unsigned_group : {false, true})
				for (unsigned size = 0; size < 4; ++size)
					for (const auto opcode : integer_opcodes) {
						const auto instruction = two_register(scalar, q,
							unsigned_group, size, opcode);
						if (valid_integer(scalar, q, unsigned_group,
						    size, opcode))
							harness.require_differential(instruction,
								test::state(opcode + size));
						else
							harness.require_illegal(instruction);
					}
		}
}

void reverse_and_byte_allocation()
{
	test::Harness harness;
	for (const bool q : {false, true}) {
		for (unsigned size = 0; size < 4; ++size) {
			const auto rev64 = two_register(false, q, false, size, 0);
			const auto rev32 = two_register(false, q, true, size, 0);
			const auto rev16 = two_register(false, q, false, size, 1);
			if (size < 3U)
				harness.require_differential(rev64, test::state(size));
			else
				harness.require_illegal(rev64);
			if (size < 2U)
				harness.require_differential(rev32, test::state(size + 4U));
			else
				harness.require_illegal(rev32);
			if (size == 0U)
				harness.require_differential(rev16, test::state(size + 8U));
			else
				harness.require_illegal(rev16);

			const auto count = two_register(false, q, false, size, 5);
			const auto invert_or_reverse = two_register(false, q, true,
				size, 5);
			if (size == 0U)
				harness.require_differential(count, test::state(12));
			else
				harness.require_illegal(count);
			if (size <= 1U)
				harness.require_differential(invert_or_reverse,
					test::state(size + 13U));
			else
				harness.require_illegal(invert_or_reverse);
		}
	}
}

void compare_zero_allocation()
{
	test::Harness harness;
	for (const bool scalar : {false, true})
		for (const bool q : {false, true}) {
			if (scalar && !q)
				continue;
			for (const bool alternate : {false, true})
				for (unsigned size = 0; size < 4; ++size)
					for (unsigned opcode = 8; opcode <= 10; ++opcode) {
						const auto instruction = two_register(scalar, q,
							alternate, size, opcode);
						const bool shape = scalar ? size == 3U :
							q || size < 3U;
						const bool operation = opcode != 10U || !alternate;
						if (shape && operation)
							harness.require_differential(instruction,
								test::state(opcode + size));
						else
							harness.require_illegal(instruction);
					}
		}
}

} // namespace

int main()
{
	integer_allocation();
	reverse_and_byte_allocation();
	compare_zero_allocation();
	return 0;
}
