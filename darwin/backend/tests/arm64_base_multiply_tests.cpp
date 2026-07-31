// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_multiply_test_support.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace multiply = uml::backend::tests::base_multiply;
namespace engine_tests = uml::backend::tests;

namespace {

[[noreturn]] void usage(const char *program)
{
	std::fprintf(stderr,
		"usage: %s [allocation|behavior|reserved]\n", program);
	std::exit(2);
}

} // namespace

int main(int argc, char **argv)
{
	engine_tests::EngineHarness harness;
	if (argc == 1) {
		multiply::run_allocation_tests(harness);
		multiply::run_behavior_tests(harness);
		multiply::run_reserved_tests(harness);
		harness.print_summary("arm64-base-multiply");
		return 0;
	}
	if (argc != 2)
		usage(argv[0]);
	if (std::strcmp(argv[1], "allocation") == 0) {
		multiply::run_allocation_tests(harness);
		harness.print_summary("arm64-base-multiply-allocation");
	} else if (std::strcmp(argv[1], "behavior") == 0) {
		multiply::run_behavior_tests(harness);
		harness.print_summary("arm64-base-multiply-behavior");
	} else if (std::strcmp(argv[1], "reserved") == 0) {
		multiply::run_reserved_tests(harness);
		harness.print_summary("arm64-base-multiply-reserved");
	} else {
		usage(argv[0]);
	}
	return 0;
}
