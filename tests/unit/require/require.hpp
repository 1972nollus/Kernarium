// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <source_location>
#include <string_view>
#include <utility>

namespace uml::testing {

/* The unit tests' assertions. A failure prints the caller's file:line and the
 * message to stderr and aborts, so neither a catch in the test nor one in the
 * code under test can swallow it, and a failure inside a callback or on
 * another thread still ends the binary. */
[[noreturn]] inline void fail(std::string_view message,
	const std::source_location location = std::source_location::current())
{
	std::fprintf(stderr, "%s:%u: %.*s\n", location.file_name(),
		static_cast<unsigned>(location.line()),
		static_cast<int>(message.size()), message.data());
	std::abort();
}

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition)
		fail("requirement failed", location);
}

inline void require(bool condition, std::string_view message,
	const std::source_location location = std::source_location::current())
{
	if (!condition)
		fail(message, location);
}

/* Requires `operation` to throw an `Exception`. Anything else it throws
 * propagates. */
template<typename Exception = std::exception, typename Operation>
void requireThrows(Operation &&operation,
	std::string_view message = "the expected exception was not thrown",
	const std::source_location location = std::source_location::current())
{
	try {
		std::forward<Operation>(operation)();
	} catch (const Exception &) {
		return;
	}
	fail(message, location);
}

} // namespace uml::testing
