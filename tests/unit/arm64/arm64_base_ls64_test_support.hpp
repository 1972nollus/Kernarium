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

namespace arm64_base_ls64_test {

namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;

enum class Operation {
	load,
	store,
	store_status,
	store_status_accdata,
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

[[nodiscard]] constexpr std::uint32_t encode(Operation operation,
	unsigned first, unsigned base, unsigned status = 31) noexcept
{
	switch (operation) {
	case Operation::load:
		return 0xf83f'd000U | ((base & 31U) << 5) | (first & 31U);
	case Operation::store:
		return 0xf83f'9000U | ((base & 31U) << 5) | (first & 31U);
	case Operation::store_status:
		return 0xf820'b000U | ((status & 31U) << 16) |
			((base & 31U) << 5) | (first & 31U);
	case Operation::store_status_accdata:
		return 0xf820'a000U | ((status & 31U) << 16) |
			((base & 31U) << 5) | (first & 31U);
	}
	return 0;
}

[[nodiscard]] constexpr bool has_status(Operation operation) noexcept
{
	return operation == Operation::store_status ||
		operation == Operation::store_status_accdata;
}

} // namespace arm64_base_ls64_test
