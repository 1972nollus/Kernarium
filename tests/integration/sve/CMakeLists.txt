# SPDX-License-Identifier: GPL-2.0-or-later

# The guest SVE tests (run_guest_sve.py). guest-sve-<engine> runs for an
# engine whose SVE support is complete, with a kernel that keeps SVE task
# state, and skips, naming the reason, otherwise; guest-sve-off-<engine>
# checks on every host that a run with the policy off shows no SVE.
uml_add_guest_payload(UML_GUEST_SVE_PROBE guest-sve-probe
    sve_probe.cpp
    COMMENT "Building the freestanding arm64 guest SVE probe"
    COMPILE_OPTIONS
        -std=c++20
        -O2
        -Wall
        -Wextra
        -Werror
        -march=armv8-a
        -mgeneral-regs-only
        -ffreestanding
        -fno-exceptions
        -fno-rtti
        -fno-stack-protector
        -fno-asynchronous-unwind-tables
        -fno-pic
    LINK_OPTIONS
        -Wl,-z,max-page-size=16384
        -Wl,-s)

# Test support only: reports each engine's SVE support level and whether
# the kernel keeps SVE task state.
add_executable(uml-sve-engine-support sve_engine_support.cpp)
target_include_directories(uml-sve-engine-support PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
target_link_libraries(uml-sve-engine-support PRIVATE
    UML::BridgeAbi
    UML::CpuContract)
add_dependencies(integration-test-binaries uml-sve-engine-support)

foreach(_engine IN ITEMS gadgets jit)
    uml_add_guest_integration_test(
        NAME guest-sve-${_engine}
        SCRIPT run_guest_sve.py
        ARGS --name guest-sve-${_engine}
            --engine ${_engine}
            --probe ${UML_GUEST_SVE_PROBE}
            --engine-support-tool $<TARGET_FILE:uml-sve-engine-support>
        LABELS guest;sve;kernel;${_engine}
        TIMEOUT 600
        CPUS 2)
    set_tests_properties(guest-sve-${_engine} PROPERTIES SKIP_RETURN_CODE 77)
    uml_add_guest_integration_test(
        NAME guest-sve-off-${_engine}
        SCRIPT run_guest_sve.py
        ARGS --name guest-sve-off-${_engine}
            --engine ${_engine}
            --probe ${UML_GUEST_SVE_PROBE}
            --modes off
        LABELS guest;sve;kernel;${_engine}
        TIMEOUT 300
        CPUS 2)
endforeach()
