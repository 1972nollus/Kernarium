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
endif()

include("${CMAKE_CURRENT_LIST_DIR}/engine.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/base.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/memory.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fp-simd-a.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fp-simd-b.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fp-simd-c.cmake")
