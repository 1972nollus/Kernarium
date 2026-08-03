// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_logical_test_support.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace logical = uml::emulator::tests::base_logical;
namespace engine_tests = uml::emulator::tests;

namespace {

[[noreturn]] void usage(const char *program)
{
	std::fprintf(stderr,
		"usage: %s [immediate|shifted|behavior]\n", program);
	std::exit(2);
}

} // namespace

int main(int argc, char **argv)
{
	engine_tests::EngineHarness harness;
	if (argc == 1) {
		logical::run_immediate_tests(harness);
		logical::run_shifted_tests(harness);
		logical::run_behavior_tests(harness);
		harness.print_summary("arm64-base-logical");
		return 0;
	}
	if (argc != 2)
		usage(argv[0]);
	if (std::strcmp(argv[1], "immediate") == 0) {
		logical::run_immediate_tests(harness);
		harness.print_summary("arm64-base-logical-immediate");
	} else if (std::strcmp(argv[1], "shifted") == 0) {
		logical::run_shifted_tests(harness);
		harness.print_summary("arm64-base-logical-shifted");
	} else if (std::strcmp(argv[1], "behavior") == 0) {
		logical::run_behavior_tests(harness);
		harness.print_summary("arm64-base-logical-behavior");
	} else {
		usage(argv[0]);
	}
	return 0;
}
