// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_system_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string_view>

namespace arm64_base_feature_system_test {

namespace engine = uml::backend::tests;
namespace sys_test = arm64_system_test;

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

class CaseName final {
public:
	template<typename... Arguments>
	explicit CaseName(const char *format, Arguments... arguments)
	{
		const int length = std::snprintf(
			storage_.data(), storage_.size(), format, arguments...);
		require(length >= 0 &&
			static_cast<std::size_t>(length) < storage_.size());
		length_ = static_cast<std::size_t>(length);
	}

	[[nodiscard]] operator std::string_view() const noexcept
	{
		return {storage_.data(), length_};
	}

private:
	std::array<char, 128> storage_{};
	std::size_t length_{};
};

inline void check_profile_illegal(
	engine::EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, engine::NativeRequirements native)
{
	const auto initial = sys_test::state();
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::illegal_instruction,
		.native = native,
	});
}

inline void check_feature_disabled(
	engine::EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, uml::backend::ArmFeature feature)
{
	check_profile_illegal(harness, name, instruction,
		engine::NativeRequirements::features(feature));
}

inline void require_counts(
	const engine::EngineHarness &harness, std::size_t cases)
{
	const auto &counts = harness.counts();
	require(counts.interpreter == cases);
	require(counts.gadgets == cases);
	require(counts.jit + counts.jit_skipped == cases);
	require(counts.jit_unavailable == 0);
}

} // namespace arm64_base_feature_system_test
