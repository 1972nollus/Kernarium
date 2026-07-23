// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_basic_floating_test_support.hpp"
#include "arm64_floating_complex_test_support.hpp"
#include "cpu/arm64/detail/floating_scale.hpp"

namespace arm64_floating_scale_test {

namespace basic = arm64_basic_floating_test;
namespace common = arm64_simd_test;
namespace complex = arm64_floating_complex_test;
namespace detail = uml::backend::arm64::detail;

using Registers = complex::Registers;
inline constexpr auto register_cases = complex::register_cases;

struct Form final {
	unsigned width;
	unsigned vector_bytes;
};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto shape : basic::vector_shapes)
		function(Form{shape[0], shape[1]});
}

[[nodiscard]] constexpr unsigned shape(const Form &form) noexcept
{
	return form.width == 16U ? (form.vector_bytes == 16U ? 1U : 0U) :
		form.width == 32U ? (form.vector_bytes == 16U ? 3U : 2U) : 4U;
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	constexpr std::array bases{0x2ec0'3c00U, 0x6ec0'3c00U,
		0x2ea0'fc00U, 0x6ea0'fc00U, 0x6ee0'fc00U};
	return bases[shape(form)] | (registers.destination & 31U) |
		((registers.first & 31U) << 5) |
		((registers.second & 31U) << 16);
}

template<typename Floating, typename Integer>
void set_scale_lane(common::arm64::CpuState &state, unsigned reg,
	unsigned lane, Integer value)
{
	basic::set_lane(state, reg, lane, std::bit_cast<Floating>(value));
}

} // namespace arm64_floating_scale_test
