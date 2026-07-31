// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_add_sub_test_support.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace add_sub = uml::backend::tests::base_add_sub;
namespace engine_tests = uml::backend::tests;

namespace {

[[noreturn]] void usage(const char *program)
{
	std::fprintf(stderr, "usage: %s [immediate|shifted|extended]\n", program);
	std::exit(2);
}

} // namespace

int main(int argc, char **argv)
{
	engine_tests::EngineHarness harness;
	if (argc == 1) {
		add_sub::run_immediate_tests(harness);
		add_sub::run_shifted_tests(harness);
		add_sub::run_extended_tests(harness);
		harness.print_summary("arm64-base-add-sub");
		return 0;
	}
	if (argc != 2)
		usage(argv[0]);
	if (std::strcmp(argv[1], "immediate") == 0) {
		add_sub::run_immediate_tests(harness);
		harness.print_summary("arm64-base-add-sub-immediate");
	} else if (std::strcmp(argv[1], "shifted") == 0) {
		add_sub::run_shifted_tests(harness);
		harness.print_summary("arm64-base-add-sub-shifted");
	} else if (std::strcmp(argv[1], "extended") == 0) {
		add_sub::run_extended_tests(harness);
		harness.print_summary("arm64-base-add-sub-extended");
	} else {
		usage(argv[0]);
	}
	return 0;
}
