# SPDX-License-Identifier: GPL-2.0-or-later

if(UML_BUILD_UNIT_TESTS AND NOT UML_GUEST_16K_PAGES)
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

if(UML_BUILD_UNIT_TESTS AND UML_GUEST_16K_PAGES AND UML_TARGET_MACOS)
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
if(_uml_unit_binary_targets)
    add_dependencies(unit-test-binaries ${_uml_unit_binary_targets})
endif()
