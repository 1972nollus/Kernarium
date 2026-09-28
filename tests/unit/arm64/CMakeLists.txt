# SPDX-License-Identifier: GPL-2.0-or-later

# ARM64 white-box tests compose the MMU OBJECT carriers directly. Carry the
# shared-backing archive that MappedBacking references so those raw
# compositions retain the production memory component's link contract.
link_libraries(UML::MemorySharedBacking)

if(NOT UML_GUEST_16K_PAGES)
    add_executable(uml_native_mmu_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/native_mmu_tests.cpp
        $<TARGET_OBJECTS:uml_native_mmu_test_variant>
        $<TARGET_OBJECTS:uml_memory_vm_bridge>
        $<TARGET_OBJECTS:uml_static_mmu_test_variant>)
    target_include_directories(uml_native_mmu_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
    target_compile_features(uml_native_mmu_tests PRIVATE cxx_std_20)
    target_compile_definitions(uml_native_mmu_tests PRIVATE
        UML_MEMORY_ALLOCATION_TESTING=1)
    target_compile_options(uml_native_mmu_tests PRIVATE
        -Wall -Wextra -Werror)
    uml_add_unit_test(NAME native-4k-shadow-mmu COMMAND uml_native_mmu_tests)
    # The test reserves the fixed Darwin native bridge window. Running after
    # several concurrent VM-heavy tests can make that host reservation fail
    # transiently even though an isolated retry succeeds.
    set_tests_properties(native-4k-shadow-mmu PROPERTIES RUN_SERIAL TRUE)

	add_executable(uml_native_4k_admission_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/native_4k_admission_tests.cpp)
	target_link_libraries(uml_native_4k_admission_tests PRIVATE
		uml_emulator_admission_test_variant)
	target_include_directories(uml_native_4k_admission_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin/emulator/src
		${PROJECT_SOURCE_DIR}/darwin/include
		${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
	target_compile_definitions(uml_native_4k_admission_tests PRIVATE
		UML_EMULATOR_ADMISSION_TESTING=1)
	target_compile_features(uml_native_4k_admission_tests PRIVATE cxx_std_20)
	target_compile_options(uml_native_4k_admission_tests PRIVATE
		-Wall -Wextra -Werror)
	uml_add_unit_test(
		NAME emulator-native-4k-admission
		COMMAND uml_native_4k_admission_tests
		LABELS tsan)
endif()

if(UML_GUEST_16K_PAGES AND UML_TARGET_MACOS)
	add_executable(uml_native_16k_ownership_tests
		${PROJECT_SOURCE_DIR}/tests/unit/arm64/native_16k_ownership_tests.cpp)
	target_link_libraries(uml_native_16k_ownership_tests PRIVATE
		uml_emulator_admission_test_variant)
	target_include_directories(uml_native_16k_ownership_tests PRIVATE
		${PROJECT_SOURCE_DIR}/darwin/include
		${PROJECT_SOURCE_DIR}/darwin/memory/mmu)
	target_compile_features(uml_native_16k_ownership_tests PRIVATE cxx_std_20)
	target_compile_options(uml_native_16k_ownership_tests PRIVATE
		-Wall -Wextra -Werror)
	uml_add_unit_test(
		NAME emulator-native-16k-ownership
		COMMAND uml_native_16k_ownership_tests
		LABELS tsan)
	# Unsupported beside AddressSanitizer's heap (native_window_test_support.hpp).
	set_tests_properties(emulator-native-16k-ownership PROPERTIES
		SKIP_RETURN_CODE 77)
endif()

# uml_add_arm64_unit_test(<stem> [NAME <test>] [WITHOUT_HARNESS]
#     [SOURCES <sources>...] [OBJECTS <sources>...] [LABELS <labels>...])
#
# Builds uml_arm64_<stem>_tests from arm64_<stem>_tests.cpp and any SOURCES,
# composed with the static MMU and gadget engine objects (or OBJECTS instead)
# and linked to the engine test harness unless WITHOUT_HARNESS, then registers
# it as arm64-<stem with dashes>, or as NAME.
function(uml_add_arm64_unit_test STEM)
    cmake_parse_arguments(PARSE_ARGV 1 _arm64_test "WITHOUT_HARNESS" "NAME"
        "SOURCES;OBJECTS;LABELS")
    set(_target uml_arm64_${STEM}_tests)
    if(NOT DEFINED _arm64_test_OBJECTS)
        set(_arm64_test_OBJECTS
            $<TARGET_OBJECTS:uml_static_mmu>
            $<TARGET_OBJECTS:uml_arm64_gadgets>
            $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    endif()
    add_executable(${_target}
        ${CMAKE_CURRENT_SOURCE_DIR}/arm64_${STEM}_tests.cpp
        ${_arm64_test_SOURCES}
        ${_arm64_test_OBJECTS})
    target_include_directories(${_target} PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(${_target} PRIVATE -Wall -Wextra -Werror)
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
