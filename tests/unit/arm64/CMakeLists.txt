# SPDX-License-Identifier: GPL-2.0-or-later

# ARM64 white-box tests compose the MMU OBJECT carriers directly. Carry the
# shared-backing archive that MappedBacking references so those raw
# compositions retain the production memory component's link contract.
link_libraries(UML::MemorySharedBacking)

# The root's warning policy covers C++; these hand-written assembly sources
# opt in to it.
set_source_files_properties(arm64_jit_chain_abi_probe.S arm64-neon-inventory.s
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

# uml_add_arm64_unit_test(<stem> [NAME <test>] [WITHOUT_HARNESS]
#     [SOURCES <sources>...] [OBJECTS <sources>... | NO_OBJECTS]
#     [LABELS <labels>...])
#
# Builds uml_arm64_<stem>_tests from arm64_<stem>_tests.cpp and any SOURCES,
# composed with the static MMU and gadget engine objects (or OBJECTS instead,
# or none for NO_OBJECTS) and linked to the engine test harness unless
# WITHOUT_HARNESS, then registers it as arm64-<stem with dashes>, or as NAME.
function(uml_add_arm64_unit_test STEM)
    cmake_parse_arguments(PARSE_ARGV 1 _arm64_test "WITHOUT_HARNESS;NO_OBJECTS"
        "NAME" "SOURCES;OBJECTS;LABELS")
    set(_target uml_arm64_${STEM}_tests)
    if(NOT DEFINED _arm64_test_OBJECTS AND NOT _arm64_test_NO_OBJECTS)
        set(_arm64_test_OBJECTS
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    endif()
    add_executable(${_target}
        ${CMAKE_CURRENT_SOURCE_DIR}/arm64_${STEM}_tests.cpp
        ${_arm64_test_SOURCES}
        ${_arm64_test_OBJECTS})
    target_link_libraries(${_target} PRIVATE UML::BridgeAbi)
    target_include_directories(${_target} PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin)
    if(NOT _arm64_test_WITHOUT_HARNESS)
        target_link_libraries(${_target} PRIVATE uml_arm64_engine_test_support)
    endif()
    if(NOT DEFINED _arm64_test_NAME)
        string(REPLACE "_" "-" _arm64_test_NAME "arm64-${STEM}")
    endif()
    set(_labels)
    if(DEFINED _arm64_test_LABELS)
        set(_labels LABELS ${_arm64_test_LABELS})
    endif()
    uml_add_unit_test(NAME ${_arm64_test_NAME} COMMAND ${_target} ${_labels})
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/engine.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/base.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/memory.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fp-simd-a.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fp-simd-b.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fp-simd-c.cmake")
