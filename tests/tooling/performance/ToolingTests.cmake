# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME gadget-dependency-analysis
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/performance/analyze_gadget_dependencies_tests.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS performance
    TIMEOUT 30)

uml_add_tooling_test(
    NAME jit-gpr-link-profile-analysis
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/performance/analyze_jit_gpr_link_profile_tests.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tests/perf
    LABELS cpu;jit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME performance-runner-cli
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/performance/test_cli.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS performance
    TIMEOUT 30)

uml_add_tooling_test(
    NAME steam-boot-bench-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/performance/steam_boot_bench_tests.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS performance;steam
    TIMEOUT 60)
