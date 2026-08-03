# SPDX-License-Identifier: GPL-2.0-or-later

if(UML_BUILD_UNIT_TESTS AND NOT UML_GUEST_16K_PAGES)
    add_executable(uml_native_mmu_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/native_mmu_tests.cpp
        $<TARGET_OBJECTS:uml_native_mmu>)
    target_include_directories(uml_native_mmu_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
    target_compile_features(uml_native_mmu_tests PRIVATE cxx_std_20)
    target_compile_options(uml_native_mmu_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME native-4k-shadow-mmu COMMAND uml_native_mmu_tests)
endif()

if(UML_BUILD_UNIT_TESTS)
    include("${CMAKE_CURRENT_LIST_DIR}/engine.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/base.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/memory.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/fp-simd-a.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/fp-simd-b.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/fp-simd-c.cmake")
endif()

get_property(_uml_unit_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(_uml_unit_binary_targets)
foreach(_target IN LISTS _uml_unit_targets)
    if(_target MATCHES "_tests$")
        list(APPEND _uml_unit_binary_targets "${_target}")
    endif()
endforeach()
if(NOT TARGET unit-test-binaries)
    add_custom_target(unit-test-binaries)
endif()
if(_uml_unit_binary_targets)
    add_dependencies(unit-test-binaries ${_uml_unit_binary_targets})
endif()
