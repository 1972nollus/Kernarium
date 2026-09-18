// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>
#include <string_view>

namespace arm64_base_gcs_test {

namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace test = uml::emulator::tests;

enum class Operation {
	pop_exception_context,
	pop_main,
	pop_exception,
	push_main,
	push_exception,
	switch_stack_1,
	switch_stack_2,
	store,
	store_unprivileged,
};

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

[[nodiscard]] constexpr std::uint32_t encode(
	Operation operation, unsigned transfer, unsigned base = 0) noexcept
{
	switch (operation) {
	case Operation::pop_exception_context:
		return 0xd508'77a0U | (transfer & 31U);
	case Operation::pop_main:
		return 0xd52b'7720U | (transfer & 31U);
	case Operation::pop_exception:
		return 0xd508'77c0U | (transfer & 31U);
	case Operation::push_main:
		return 0xd50b'7700U | (transfer & 31U);
	case Operation::push_exception:
		return 0xd508'7780U | (transfer & 31U);
	case Operation::switch_stack_1:
		return 0xd50b'7740U | (transfer & 31U);
	case Operation::switch_stack_2:
		return 0xd52b'7760U | (transfer & 31U);
	case Operation::store:
		return 0xd91f'0c00U | ((base & 31U) << 5) |
			(transfer & 31U);
	case Operation::store_unprivileged:
		return 0xd91f'1c00U | ((base & 31U) << 5) |
			(transfer & 31U);
	}
	return 0;
}

} // namespace arm64_base_gcs_test
