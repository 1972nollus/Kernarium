// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_hint_test_support.hpp"

#include <cerrno>
#include <cstdlib>
#include <limits>

namespace {

constexpr unsigned shard_count = 8;
constexpr unsigned immediate_count =
	std::numeric_limits<std::uint16_t>::max() + 1U;

[[nodiscard]] unsigned selected_shard(int argc, char **argv)
{
	if (argc == 1)
		return shard_count;
	if (argc != 2)
		arm64_system_test::require(false);
	char *end = nullptr;
	errno = 0;
	const auto value = std::strtoul(argv[1], &end, 10);
	arm64_system_test::require(errno == 0 && end != argv[1] &&
		*end == '\0' && value < shard_count);
	return static_cast<unsigned>(value);
}

void extended_hints(unsigned shard)
{
	arm64_hint_test::Harness harness;
	const auto native =
		uml::emulator::tests::NativeRequirements::features(
			::uml::cpu::ArmFeature::hinte);
	const unsigned begin = shard == shard_count ? 0 :
		shard * (immediate_count / shard_count);
	const unsigned end = shard == shard_count ? immediate_count :
		begin + immediate_count / shard_count;
	for (unsigned immediate = begin; immediate < end; ++immediate) {
		if (arm64_hint_test::is_primary_hint(immediate))
			continue;
		const auto instruction =
			arm64_hint_test::extended_instruction(immediate);
		harness.check("BASE-0176 HINTE", instruction, native);
	}
	harness.print_summary("arm64-base-extended-hints");
}

} // namespace

int main(int argc, char **argv)
{
	extended_hints(selected_shard(argc, argv));
	return 0;
}
