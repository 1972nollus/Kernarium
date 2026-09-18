// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_basic_floating_test_support.hpp"
#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <type_traits>

namespace arm64_floating_complex_test {

namespace basic = arm64_basic_floating_test;
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	add,
	multiply_accumulate,
};

struct Form final {
	Operation operation;
	bool by_element;
	unsigned width;
	unsigned vector_bytes;
	unsigned rotation;
	unsigned index;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto shape : basic::vector_shapes)
		for (const unsigned rotation : {1U, 3U})
			function(Form{Operation::add, false, shape[0], shape[1],
				rotation, 0U});
	for (const auto shape : basic::vector_shapes)
		for (unsigned rotation = 0; rotation < 4U; ++rotation)
			function(Form{Operation::multiply_accumulate, false,
				shape[0], shape[1], rotation, 0U});
	for (unsigned index = 0; index < 2U; ++index)
		for (unsigned rotation = 0; rotation < 4U; ++rotation)
			function(Form{Operation::multiply_accumulate, true,
				16U, 8U, rotation, index});
	for (unsigned index = 0; index < 4U; ++index)
		for (unsigned rotation = 0; rotation < 4U; ++rotation)
			function(Form{Operation::multiply_accumulate, true,
				16U, 16U, rotation, index});
	for (unsigned index = 0; index < 2U; ++index)
		for (unsigned rotation = 0; rotation < 4U; ++rotation)
			function(Form{Operation::multiply_accumulate, true,
				32U, 16U, rotation, index});
}

[[nodiscard]] constexpr unsigned vector_shape(unsigned width,
	unsigned vector_bytes) noexcept
{
	return width == 16U ? (vector_bytes == 16U ? 1U : 0U) :
		width == 32U ? (vector_bytes == 16U ? 3U : 2U) : 4U;
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	constexpr std::array add{
		0x2e40'e400U, 0x6e40'e400U, 0x2e80'e400U,
		0x6e80'e400U, 0x6ec0'e400U,
	};
	constexpr std::array multiply{
		0x2e40'c400U, 0x6e40'c400U, 0x2e80'c400U,
		0x6e80'c400U, 0x6ec0'c400U,
	};
	std::uint32_t value;
	if (form.operation == Operation::add) {
		value = add[vector_shape(form.width, form.vector_bytes)];
		if (form.rotation == 3U)
			value |= 0x1000U;
	} else if (!form.by_element) {
		value = multiply[vector_shape(form.width, form.vector_bytes)] |
			(form.rotation << 11);
	} else {
		value = form.width == 32U ? 0x6f80'1000U :
			form.vector_bytes == 16U ? 0x6f40'1000U :
			0x2f40'1000U;
		if (form.width == 16U) {
			value |= (form.index & 1U) << 21;
			value |= (form.index >> 1) << 11;
		} else {
			value |= form.index << 11;
		}
		value |= form.rotation << 13;
	}
	value |= registers.destination & 31U;
	value |= (registers.first & 31U) << 5;
	if (form.by_element) {
		value |= (registers.second & 15U) << 16;
		value |= ((registers.second >> 4) & 1U) << 20;
	} else {
		value |= (registers.second & 31U) << 16;
	}
	return value;
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(
	unsigned reg) noexcept
{
	return offsetof(arm64::CpuState, vectors) +
		reg * sizeof(arm64::VectorRegister);
}

[[nodiscard]] constexpr std::uintptr_t gadget_operand(
	Registers registers) noexcept
{
	static_assert(vector_offset(31U) <= 0xffffU);
	return vector_offset(registers.destination) |
		(vector_offset(registers.first) << 16U) |
		(vector_offset(registers.second) << 32U);
}

} // namespace arm64_floating_complex_test
